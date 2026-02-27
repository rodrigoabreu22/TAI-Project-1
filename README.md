TAI - Project 1

## Build Binaries
```bash
cmake -S . -B build
cmake --build build -j
```

## Run Encoders and Decoders

### Algorithm 1 (AI Generated)
```bash
./build/g3_v1_c data/A results/A.arith
./build/g3_v1_d results/A.arith results/A.dec
```

### Algorithm 2 (https://github.com/tazik/compressor/blob/master/compress.cpp)
```bash
./build/g3_v2_c data/A results/A.arith2
./build/g3_v2_d results/A.arith2 results/A.dec
```
