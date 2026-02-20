#!/usr/bin/env bash
# =============================================================================
# benchmark.sh — Automated compression benchmark
# Replicates and extends the professor's benchmark table.
#
# Usage:
#   ./benchmark.sh [OPTIONS]
#
# Options:
#   -d DIR        Data directory (default: data)
#   -f FILES      Comma-separated file names to test (default: A,B,C,D,E,F,G,H)
#   -c            Test on concatenated file (all files joined — matches prof table)
#   -o TOOL_CMD   Add your own tool. Format: "name:compress_cmd:decompress_cmd"
#                 Example: -o "ox:./ox -c:./ox -d"
#   -r RUNS       Number of timing runs per compressor (default: 1)
#   -q            Quiet — suppress progress output, only show final table
#   -h            Show this help
#
# Examples:
#   ./benchmark.sh                          # test all files individually
#   ./benchmark.sh -c                       # concatenate A-H (matches prof table)
#   ./benchmark.sh -c -o "ox:./ox -c:./ox -d"
#   ./benchmark.sh -f C,D -r 3             # test files C and D, 3 timing runs
# =============================================================================

set -euo pipefail

# ---------- defaults ---------------------------------------------------------
DATA_DIR="data"
FILES_ARG="A,B,C,D,E,F,G,H"
CONCAT_MODE=false
OWN_TOOL=""
RUNS=1
QUIET=false

# ---------- ANSI colours -----------------------------------------------------
R='\033[0;31m'; G='\033[0;32m'; Y='\033[1;33m'
B='\033[0;34m'; C='\033[0;36m'; W='\033[1;37m'; N='\033[0m'

log()  { $QUIET || printf "${C}[bench]${N} %s\n" "$*"; }
warn() { printf "${Y}[warn] ${N}%s\n" "$*" >&2; }
err()  { printf "${R}[error]${N} %s\n" "$*" >&2; exit 1; }

# ---------- arg parsing ------------------------------------------------------
while getopts "d:f:co:r:qh" opt; do
    case $opt in
        d) DATA_DIR="$OPTARG" ;;
        f) FILES_ARG="$OPTARG" ;;
        c) CONCAT_MODE=true ;;
        o) OWN_TOOL="$OPTARG" ;;
        r) RUNS="$OPTARG" ;;
        q) QUIET=true ;;
        h) sed -n '2,30p' "$0"; exit 0 ;;
        *) err "Unknown option -$OPTARG. Use -h for help." ;;
    esac
done

# ---------- dependencies check -----------------------------------------------
for bin in gzip bzip2 xz zstd bc md5sum; do
    command -v "$bin" &>/dev/null || err "Required tool not found: $bin"
done
# /usr/bin/time (not the shell builtin) needed for -f flag
TIME_CMD="/usr/bin/time"
[[ -x "$TIME_CMD" ]] || err "/usr/bin/time not found (needed for timing)"

# ---------- compressor definitions -------------------------------------------
# Arrays: NAMES, COMP_CMDS, DECOMP_CMDS
NAMES=()
COMP_CMDS=()
DECOMP_CMDS=()

add_compressor() {
    NAMES+=("$1")
    COMP_CMDS+=("$2")
    DECOMP_CMDS+=("$3")
}

add_compressor "gzip"     "gzip -6 -c"          "gzip -d -c"
add_compressor "bzip2"    "bzip2 -9 -c"          "bzip2 -d -c"
add_compressor "lzma-1"   "xz -1 --format=lzma -c" "xz -d -c"
add_compressor "lzma-5"   "xz -5 --format=lzma -c" "xz -d -c"
add_compressor "lzma-9"   "xz -9 --format=lzma -c" "xz -d -c"
add_compressor "xz-6"     "xz -6 -c"             "xz -d -c"
add_compressor "zstd-1"   "zstd -1 -q -c"        "zstd -d -q -c"
add_compressor "zstd-3"   "zstd -3 -q -c"        "zstd -d -q -c"
add_compressor "zstd-19"  "zstd -19 -q -c"       "zstd -d -q -c"

# Append user's own tool if provided
if [[ -n "$OWN_TOOL" ]]; then
    IFS=':' read -r own_name own_comp own_decomp <<< "$OWN_TOOL"
    add_compressor "$own_name" "$own_comp" "$own_decomp"
fi

# ---------- helpers ----------------------------------------------------------
# All floating-point formatting goes through awk with OFMT to stay locale-safe.
bytes_to_mb()  { awk "BEGIN{printf \"%.2f\", $1/1048576}"; }
bits_per_sym() { awk "BEGIN{printf \"%.3f\", ($2*8)/$1}"; }
ratio_pct()    { awk "BEGIN{printf \"%.1f\", $2*100/$1}"; }
fmt3()         { awk "BEGIN{printf \"%.3f\", $1}"; }   # format to 3 dp, leading zero

# Run a command, measure wall-clock time in seconds, return time via stdout
# Usage: elapsed=$( time_cmd CMD [ARGS...] )
time_cmd() {
    local t
    # /usr/bin/time writes to stderr; redirect to a temp file
    local tf
    tf=$(mktemp)
    "$TIME_CMD" -f "%e" -o "$tf" "$@" > /dev/null 2>&1 || {
        rm -f "$tf"
        echo "FAILED"
        return 1
    }
    t=$(cat "$tf")
    rm -f "$tf"
    echo "$t"
}

# Average over multiple runs
avg_time() {
    local cmd=("$@")
    local sum=0
    local i
    for (( i=0; i<RUNS; i++ )); do
        local t
        t=$( "$TIME_CMD" -f "%e" "${cmd[@]}" 2>&1 >/dev/null ) || { echo "FAILED"; return 1; }
        sum=$(echo "$sum + $t" | bc)
    done
    echo "scale=3; $sum / $RUNS" | bc
}

# ---------- benchmark one file with one compressor ---------------------------
# Outputs one result row: comp_bytes t_comp t_decomp lossless
bench_one() {
    local input="$1" name="$2" comp_cmd="$3" decomp_cmd="$4"
    local tmpcomp tmpdecomp tfile
    tmpcomp=$(mktemp)
    tmpdecomp=$(mktemp)
    tfile=$(mktemp)

    # --- compress ---
    local t_comp
    local comp_ok=true
    if ! "$TIME_CMD" -f "%e" -o "$tfile" bash -c "$comp_cmd < \"\$0\" > \"\$1\"" \
            "$input" "$tmpcomp" 2>/dev/null; then
        comp_ok=false
    fi
    t_comp=$(cat "$tfile")

    # Average over more runs if requested
    if [[ "$RUNS" -gt 1 ]] && $comp_ok; then
        local sum_c=0
        local i
        for (( i=0; i<RUNS; i++ )); do
            if "$TIME_CMD" -f "%e" -o "$tfile" bash -c "$comp_cmd < \"\$0\" > \"\$1\"" \
                    "$input" "$tmpcomp" 2>/dev/null; then
                local t
                t=$(cat "$tfile")
                sum_c=$(echo "$sum_c + $t" | bc)
            fi
        done
        t_comp=$(echo "scale=3; $sum_c / $RUNS" | bc)
    fi

    local comp_bytes
    comp_bytes=$(stat -c%s "$tmpcomp" 2>/dev/null || echo 0)

    # --- decompress ---
    local t_decomp="0.000"
    local lossless="NO"
    if $comp_ok && [[ "$comp_bytes" -gt 0 ]]; then
        if "$TIME_CMD" -f "%e" -o "$tfile" bash -c "$decomp_cmd < \"\$0\" > \"\$1\"" \
                "$tmpcomp" "$tmpdecomp" 2>/dev/null; then
            t_decomp=$(cat "$tfile")

            if [[ "$RUNS" -gt 1 ]]; then
                local sum_d=0
                local i
                for (( i=0; i<RUNS; i++ )); do
                    if "$TIME_CMD" -f "%e" -o "$tfile" bash -c "$decomp_cmd < \"\$0\" > \"\$1\"" \
                            "$tmpcomp" "$tmpdecomp" 2>/dev/null; then
                        local t
                        t=$(cat "$tfile")
                        sum_d=$(echo "$sum_d + $t" | bc)
                    fi
                done
                t_decomp=$(echo "scale=3; $sum_d / $RUNS" | bc)
            fi

            # Lossless check via md5
            local md5_orig md5_decomp
            md5_orig=$(md5sum "$input" | cut -d' ' -f1)
            md5_decomp=$(md5sum "$tmpdecomp" | cut -d' ' -f1)
            [[ "$md5_orig" == "$md5_decomp" ]] && lossless="YES" || lossless="NO"
        fi
    fi

    rm -f "$tmpcomp" "$tmpdecomp" "$tfile"
    echo "$comp_bytes $t_comp $t_decomp $lossless"
}

# ---------- print a separator ------------------------------------------------
separator() {
    printf '+------+%-16s+%-14s+%-14s+--------+-----------+-----------+-----------+-----------+-----------+\n' \
        "----------------" "--------------" "--------------"
}

# ---------- print table header -----------------------------------------------
print_header() {
    local label="$1"
    echo ""
    printf "${W}%s${N}\n" "$label"
    separator
    printf '| %-4s | %-14s | %-12s | %-12s | %-6s | %-9s | %-9s | %-9s | %-9s | %-9s |\n' \
        "Rank" "Compressor" "Original(MB)" "Comprssd(MB)" "Ratio%" \
        "bits/byte" "t_comp(s)" "t_dcp(s)" "t_total(s)" "Lossless"
    separator
}

# ---------- collect results for one input file and print table ---------------
bench_file() {
    local input="$1" label="$2"

    local orig_bytes
    orig_bytes=$(stat -c%s "$input")
    local orig_mb
    orig_mb=$(bytes_to_mb "$orig_bytes")

    log "Benchmarking: $label  ($(printf '%s' "$orig_mb") MB, $orig_bytes bytes)"

    # Collect rows: name comp_bytes t_comp t_decomp lossless
    local -a rows_name rows_comp rows_tc rows_td rows_lossless

    local i
    for (( i=0; i<${#NAMES[@]}; i++ )); do
        local name="${NAMES[$i]}"
        local cc="${COMP_CMDS[$i]}"
        local dc="${DECOMP_CMDS[$i]}"
        log "  -> $name ..."
        read -r cb tc td ls <<< "$(bench_one "$input" "$name" "$cc" "$dc")"
        rows_name+=("$name")
        rows_comp+=("$cb")
        rows_tc+=("$tc")
        rows_td+=("$td")
        rows_lossless+=("$ls")
    done

    # Sort by compressed size (ascending) to get rank
    local -a order
    # Create sortable list: "comp_bytes index"
    local sortlist=""
    for (( i=0; i<${#rows_name[@]}; i++ )); do
        sortlist+="${rows_comp[$i]} $i"$'\n'
    done
    mapfile -t sorted <<< "$(echo "$sortlist" | sort -n)"

    print_header "$label"

    local rank=1
    for entry in "${sorted[@]}"; do
        [[ -z "$entry" ]] && continue
        local idx
        idx=$(echo "$entry" | awk '{print $2}')
        local name="${rows_name[$idx]}"
        local cb="${rows_comp[$idx]}"
        local tc="${rows_tc[$idx]}"
        local td="${rows_td[$idx]}"
        local ls="${rows_lossless[$idx]}"

        local comp_mb ratio bps tt
        comp_mb=$(bytes_to_mb "$cb")
        ratio=$(ratio_pct "$orig_bytes" "$cb")
        bps=$(bits_per_sym "$orig_bytes" "$cb")
        tc=$(fmt3 "$tc")
        td=$(fmt3 "$td")
        tt=$(awk "BEGIN{printf \"%.3f\", $tc+$td}")

        local lossless_mark
        [[ "$ls" == "YES" ]] && lossless_mark="${G}YES${N}" || lossless_mark="${R}NO ${N}"

        printf "| %-4s | %-14s | %12s | %12s | %6s | %9s | %9s | %9s | %9s | %-9b |\n" \
            "$rank" "$name" "$orig_mb" "$comp_mb" "${ratio}%" \
            "$bps" "$tc" "$td" "$tt" "$lossless_mark"

        (( rank++ ))
    done

    separator
    echo ""
}

# ---------- main -------------------------------------------------------------
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

IFS=',' read -ra FILE_LIST <<< "$FILES_ARG"

# Validate files exist
for f in "${FILE_LIST[@]}"; do
    fp="$DATA_DIR/$f"
    [[ -f "$fp" ]] || err "File not found: $fp"
done

echo ""
printf "${W}========================================================${N}\n"
printf "${W}  TAI Project 1 — Compression Benchmark${N}\n"
printf "${W}  Runs per compressor: $RUNS${N}\n"
printf "${W}========================================================${N}\n"

if $CONCAT_MODE; then
    # Concatenate all selected files into one
    cat_file="$WORK_DIR/concat"
    log "Concatenating files: ${FILE_LIST[*]}"
    for f in "${FILE_LIST[@]}"; do
        cat "$DATA_DIR/$f" >> "$cat_file"
    done
    label="Concatenated ($(IFS=+; echo "${FILE_LIST[*]}"))"
    bench_file "$cat_file" "$label"
else
    for f in "${FILE_LIST[@]}"; do
        bench_file "$DATA_DIR/$f" "File $f"
    done
fi

printf "${W}Done.${N}\n\n"
