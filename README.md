# TAI - Project 1


## How to run
- build
```bash
cmake --build build -j$(nproc)
```

- run 
```bash
./benchmark.sh -m -o "ox:./build/compress %i %o:./build/decompress %i %o"
```

- run (ratio-focused compressor)
```bash
./benchmark.sh -m -o "ox:./build/compress_ratio %i %o:./build/decompress_ratio %i %o"
```

The ratio-focused codec is a separate block-BWT + context-mixing path and uses
its own `TA10` file format.
