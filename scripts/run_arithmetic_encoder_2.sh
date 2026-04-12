#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root_dir="$(cd "$script_dir/.." && pwd)"
results_dir="$root_dir/results"
mkdir -p "$results_dir"

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "Usage: $0 <input_file> [output_file]" >&2
  echo "   or: $0 -d <input_file> <output_file>" >&2
  exit 1
fi

mode="compress"
if [[ "$1" == "-d" ]]; then
  mode="decompress"
  shift
fi

input="$1"
default_name="$(basename "$input").arith2"
output_name="${2:-$default_name}"
output="$results_dir/$(basename "$output_name")"
if [[ "$mode" == "decompress" && $# -ne 2 ]]; then
  echo "Usage: $0 -d <input_file> <output_file>" >&2
  exit 1
fi

if [[ ! -f "$input" ]]; then
  echo "Input file not found: $input" >&2
  exit 1
fi

# Build binary if missing or older than source
bin="$root_dir/arithmetic_encoder_2"
src="$root_dir/src/coder/arithmetic_encoder_2.cpp"
if [[ ! -x "$bin" || "$src" -nt "$bin" ]]; then
  g++ -std=c++17 -O3 -o "$bin" "$src"
fi

if [[ "$mode" == "decompress" ]]; then
  "$bin" d "$input" "$output"
else
  "$bin" c "$input" "$output"
fi

echo "Wrote: $output"
