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