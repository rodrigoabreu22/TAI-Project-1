// rANS decoder based on rygorous/ryg_rans (public domain).

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// ---- Begin rans_byte.h (public domain, Fabian 'ryg' Giesen 2014) ----
#ifndef RANS_BYTE_HEADER
#define RANS_BYTE_HEADER

#include <assert.h>

#ifdef assert
#define RansAssert assert
#else
#define RansAssert(x) do { (void)(x); } while (0)
#endif

#define RANS_BYTE_L (1u << 23)

typedef uint32_t RansState;

static inline void RansDecInit(RansState* r, uint8_t** pptr) {
    uint32_t x;
    uint8_t* ptr = *pptr;
    x = ptr[0] << 0;
    x |= ptr[1] << 8;
    x |= ptr[2] << 16;
    x |= ptr[3] << 24;
    ptr += 4;
    *pptr = ptr;
    *r = x;
}

static inline uint32_t RansDecGet(RansState* r, uint32_t scale_bits) {
    return *r & ((1u << scale_bits) - 1);
}

static inline void RansDecAdvance(RansState* r, uint8_t** pptr, uint32_t start, uint32_t freq, uint32_t scale_bits) {
    uint32_t mask = (1u << scale_bits) - 1;
    uint32_t x = *r;
    x = freq * (x >> scale_bits) + (x & mask) - start;
    if (x < RANS_BYTE_L) {
        uint8_t* ptr = *pptr;
        do x = (x << 8) | *ptr++;
        while (x < RANS_BYTE_L);
        *pptr = ptr;
    }
    *r = x;
}

typedef struct {
    uint16_t start;
    uint16_t freq;
} RansDecSymbol;

static inline void RansDecSymbolInit(RansDecSymbol* s, uint32_t start, uint32_t freq) {
    RansAssert(start <= (1 << 16));
    RansAssert(freq <= (1 << 16) - start);
    s->start = (uint16_t)start;
    s->freq = (uint16_t)freq;
}

static inline void RansDecAdvanceSymbol(RansState* r, uint8_t** pptr, RansDecSymbol const* sym, uint32_t scale_bits) {
    RansDecAdvance(r, pptr, sym->start, sym->freq, scale_bits);
}

#endif // RANS_BYTE_HEADER
// ---- End rans_byte.h ----

static uint8_t read_u8(std::istream& in) {
    int c = in.get();
    if (c == EOF) throw std::runtime_error("Unexpected EOF");
    return static_cast<uint8_t>(c);
}

static uint64_t read_u64(std::istream& in) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        int c = in.get();
        if (c == EOF) throw std::runtime_error("Unexpected EOF");
        v |= static_cast<uint64_t>(static_cast<uint8_t>(c)) << (8 * i);
    }
    return v;
}

static uint16_t read_u16(std::istream& in) {
    int c0 = in.get();
    int c1 = in.get();
    if (c0 == EOF || c1 == EOF) throw std::runtime_error("Unexpected EOF");
    return static_cast<uint16_t>(static_cast<uint8_t>(c0)) |
           (static_cast<uint16_t>(static_cast<uint8_t>(c1)) << 8);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file>" << std::endl;
        return 1;
    }

    try {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open input file");

        char magic[4];
        in.read(magic, 4);
        if (in.gcount() != 4 || std::string(magic, 4) != "RANS")
            throw std::runtime_error("Invalid file format");

        uint8_t prob_bits = read_u8(in);
        if (prob_bits == 0 || prob_bits > 16) throw std::runtime_error("Invalid prob bits");
        uint32_t prob_scale = 1u << prob_bits;

        uint64_t original_size = read_u64(in);

        uint32_t freqs[256];
        for (int i = 0; i < 256; i++) {
            freqs[i] = read_u16(in);
            if (freqs[i] == 0) throw std::runtime_error("Invalid frequency table");
        }

        uint32_t cum_freqs[257];
        cum_freqs[0] = 0;
        for (int i = 0; i < 256; i++) cum_freqs[i + 1] = cum_freqs[i] + freqs[i];
        if (cum_freqs[256] != prob_scale) throw std::runtime_error("Frequency sum mismatch");

        RansDecSymbol dsyms[256];
        for (int i = 0; i < 256; i++) RansDecSymbolInit(&dsyms[i], cum_freqs[i], freqs[i]);

        std::vector<uint8_t> comp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (comp.size() < 4) throw std::runtime_error("Corrupt compressed data");

        uint8_t* ptr = comp.data();
        RansState rans;
        RansDecInit(&rans, &ptr);

        std::vector<uint8_t> output;
        output.resize(static_cast<size_t>(original_size));

        // Build cum2sym table
        std::vector<uint8_t> cum2sym(prob_scale);
        for (int i = 0; i < 256; i++) {
            for (uint32_t j = cum_freqs[i]; j < cum_freqs[i + 1]; j++)
                cum2sym[j] = static_cast<uint8_t>(i);
        }

        for (uint64_t i = 0; i < original_size; i++) {
            uint32_t cum = RansDecGet(&rans, prob_bits);
            uint8_t sym = cum2sym[cum];
            output[i] = sym;
            RansDecAdvanceSymbol(&rans, &ptr, &dsyms[sym], prob_bits);
        }

        std::ofstream out(argv[2], std::ios::binary);
        if (!out) throw std::runtime_error("Cannot create output file");
        out.write(reinterpret_cast<const char*>(output.data()), output.size());

        std::cout << "Decompressed to: " << argv[2] << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
