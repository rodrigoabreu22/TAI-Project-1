# TAI - Project 1
**Algorithmic Information Theory (2025/26) — Universidade of Aveiro**

Lossless data compression tool implementing a two-stage architecture: a custom **modeling stage** and an arithmetic/range **coding stage**. Three independent compressors are provided, each targeting a different trade-off between compression ratio and speed.

| Tool | Strategy |
|------|----------|
| `ox-balanced` | Best ratio within a tight time budget (BWT + MTF + adaptive range coder) |
| `ox-ratio`    | Maximum compression ratio regardless of time |
| `ox-speed`    | Minimum compression/decompression latency |

## Build

```bash
# Configure (first time only)
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build -j$(nproc)
```

Produces binaries in `build/`:
- `compress_balanced` / `decompress_balanced`
- `compress_ratio` / `decompress_ratio` 
- `compress_fast` / `decompress_fast` 

## Usage

```bash
# Compress
./build/compress_balanced <input_file> <output_file>

# Decompress
./build/decompress_balanced <compressed_file> <output_file>
```

## Benchmark

Run against `gzip`, `bzip2`, `lzma`, `xz`, and `zstd` (per-file mean over files A–H):

```bash
# Only one
./benchmark.sh -m -q -o "ox-balanced:./build/compress_balanced %i %o:./build/decompress_balanced %i %o"

# all three
./benchmark.sh -m -q \
  -o "ox-balanced:./build/compress_balanced %i %o:./build/decompress_balanced %i %o" \
  -o "ox-ratio:./build/compress_ratio %i %o:./build/decompress_ratio %i %o" \
  -o "ox-fast:./build/compress_fast %i %o:./build/decompress_fast %i %o"
```

Benchmark results are recorded in [`benchmarks.md`](benchmarks.md).

## Project Structure

```
src/
  balanced/              # Balanced compressor (BWT + MTF + range coder)
    main_compress.cpp
    main_decompress.cpp
    model/               # BWT (SA-IS), RLE
    coder/               # Range coder, Fenwick frequency table
  fast/                  # Speed-focused compressor 
  ratio/                 # Ratio-focused compressor 
data/                    # Benchmark files A–H (gitignored)
build/                   # Compiled binaries (gitignored)
benchmarks.md            # Benchmark results
```
