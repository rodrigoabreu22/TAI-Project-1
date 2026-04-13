# TAI - Project 1

Lossless compressor using **Block BWT + MTF + Context Mixing + Range Coding**.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Run

```bash
# Compress
./build/compress <input_file> <output_file>

# Decompress
./build/decompress <compressed_file> <output_file>
```

Both tools also accept stdin/stdout when called with no arguments.

## Benchmark

```bash
./benchmark.sh -m -o "ox:./build/compress %i %o:./build/decompress %i %o"
```

## Algorithm overview

1. **BWT** — Burrows-Wheeler Transform clusters similar bytes together (SA-IS, O(N))
2. **MTF** — Move-to-Front converts clustered bytes to small integers (many zeros)
3. **Context Mixing** — 8 adaptive models predict each bit in logit space:
   - Stationary (global bit stats)
   - ctx0–ctx4 (1 to 4-byte history context maps)
   - Run-length context
   - Match model (LZ-style history match)
4. **SSE** — Two-layer Secondary Symbol Estimation calibrates the mixed probability
5. **Range Coder** — encodes each bit using near-optimal fractional bits based on the model's confidence
