// rANS encoder based on rygorous/ryg_rans (public domain).
// Uses byte-aligned rANS with a static frequency table per file.

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

static inline void RansEncInit(RansState* r) {
    *r = RANS_BYTE_L;
}

static inline RansState RansEncRenorm(RansState x, uint8_t** pptr, uint32_t freq, uint32_t scale_bits) {
    uint32_t x_max = ((RANS_BYTE_L >> scale_bits) << 8) * freq;
    if (x >= x_max) {
        uint8_t* ptr = *pptr;
        do {
            *--ptr = (uint8_t)(x & 0xff);
            x >>= 8;
        } while (x >= x_max);
        *pptr = ptr;
    }
    return x;
}

static inline void RansEncPut(RansState* r, uint8_t** pptr, uint32_t start, uint32_t freq, uint32_t scale_bits) {
    RansState x = RansEncRenorm(*r, pptr, freq, scale_bits);
    *r = ((x / freq) << scale_bits) + (x % freq) + start;
}

static inline void RansEncFlush(RansState* r, uint8_t** pptr) {
    uint32_t x = *r;
    uint8_t* ptr = *pptr;
    ptr -= 4;
    ptr[0] = (uint8_t)(x >> 0);
    ptr[1] = (uint8_t)(x >> 8);
    ptr[2] = (uint8_t)(x >> 16);
    ptr[3] = (uint8_t)(x >> 24);
    *pptr = ptr;
}

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
    uint32_t x_max;
    uint32_t rcp_freq;
    uint32_t bias;
    uint16_t cmpl_freq;
    uint16_t rcp_shift;
} RansEncSymbol;

typedef struct {
    uint16_t start;
    uint16_t freq;
} RansDecSymbol;

static inline void RansEncSymbolInit(RansEncSymbol* s, uint32_t start, uint32_t freq, uint32_t scale_bits) {
    RansAssert(scale_bits <= 16);
    RansAssert(start <= (1u << scale_bits));
    RansAssert(freq <= (1u << scale_bits) - start);
    s->x_max = ((RANS_BYTE_L >> scale_bits) << 8) * freq;
    s->cmpl_freq = (uint16_t)((1 << scale_bits) - freq);
    if (freq < 2) {
        s->rcp_freq = ~0u;
        s->rcp_shift = 0;
        s->bias = start + (1 << scale_bits) - 1;
    } else {
        uint32_t shift = 0;
        while (freq > (1u << shift)) shift++;
        s->rcp_freq = (uint32_t)(((1ull << (shift + 31)) + freq - 1) / freq);
        s->rcp_shift = shift - 1;
        s->bias = start;
    }
}

static inline void RansDecSymbolInit(RansDecSymbol* s, uint32_t start, uint32_t freq) {
    RansAssert(start <= (1 << 16));
    RansAssert(freq <= (1 << 16) - start);
    s->start = (uint16_t)start;
    s->freq = (uint16_t)freq;
}

static inline void RansEncPutSymbol(RansState* r, uint8_t** pptr, RansEncSymbol const* sym) {
    RansAssert(sym->x_max != 0);
    uint32_t x = *r;
    uint32_t x_max = sym->x_max;
    if (x >= x_max) {
        uint8_t* ptr = *pptr;
        do {
            *--ptr = (uint8_t)(x & 0xff);
            x >>= 8;
        } while (x >= x_max);
        *pptr = ptr;
    }
    uint32_t q = (uint32_t)(((uint64_t)x * sym->rcp_freq) >> 32) >> sym->rcp_shift;
    *r = x + sym->bias + q * sym->cmpl_freq;
}

static inline void RansDecAdvanceSymbol(RansState* r, uint8_t** pptr, RansDecSymbol const* sym, uint32_t scale_bits) {
    RansDecAdvance(r, pptr, sym->start, sym->freq, scale_bits);
}

#endif // RANS_BYTE_HEADER
// ---- End rans_byte.h ----

static constexpr uint32_t PROB_BITS = 14;
static constexpr uint32_t PROB_SCALE = 1u << PROB_BITS;

struct SymbolStats {
    uint32_t freqs[256];
    uint32_t cum_freqs[257];

    void count_freqs(const std::vector<uint8_t>& data) {
        std::fill(std::begin(freqs), std::end(freqs), 0u);
        for (uint8_t b : data) freqs[b]++;
    }

    void calc_cum_freqs() {
        cum_freqs[0] = 0;
        for (int i = 0; i < 256; i++) cum_freqs[i + 1] = cum_freqs[i] + freqs[i];
    }

    void normalize_freqs(uint32_t target_total) {
        calc_cum_freqs();
        uint32_t cur_total = cum_freqs[256];
        if (cur_total == 0) {
            // Empty input: make a flat distribution
            for (int i = 0; i < 256; i++) freqs[i] = 1;
            calc_cum_freqs();
            return;
        }

        for (int i = 1; i <= 256; i++)
            cum_freqs[i] = (uint64_t)target_total * cum_freqs[i] / cur_total;

        for (int i = 0; i < 256; i++) {
            if (freqs[i] && cum_freqs[i + 1] == cum_freqs[i]) {
                uint32_t best_freq = ~0u;
                int best_steal = -1;
                for (int j = 0; j < 256; j++) {
                    uint32_t freq = cum_freqs[j + 1] - cum_freqs[j];
                    if (freq > 1 && freq < best_freq) {
                        best_freq = freq;
                        best_steal = j;
                    }
                }
                if (best_steal < 0) throw std::runtime_error("Frequency normalization failed");
                if (best_steal < i) {
                    for (int j = best_steal + 1; j <= i; j++) cum_freqs[j]--;
                } else {
                    for (int j = i + 1; j <= best_steal; j++) cum_freqs[j]++;
                }
            }
        }

        for (int i = 0; i < 256; i++) {
            uint32_t f = cum_freqs[i + 1] - cum_freqs[i];
            if (freqs[i] != 0 && f == 0) {
                throw std::runtime_error("Normalization produced zero frequency");
            }
            freqs[i] = f;
        }
    }
};

static void write_u8(std::ostream& out, uint8_t v) {
    out.put(static_cast<char>(v));
}

static void write_u64(std::ostream& out, uint64_t v) {
    for (int i = 0; i < 8; i++) out.put(static_cast<char>((v >> (8 * i)) & 0xFF));
}

static void write_u16(std::ostream& out, uint16_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file>" << std::endl;
        return 1;
    }

    try {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open input file");
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        SymbolStats stats;
        stats.count_freqs(data);
        stats.normalize_freqs(PROB_SCALE);
        stats.calc_cum_freqs();

        RansEncSymbol esyms[256];
        for (int i = 0; i < 256; i++)
            RansEncSymbolInit(&esyms[i], stats.cum_freqs[i], stats.freqs[i], PROB_BITS);

        size_t out_max = data.size() + 1024 + data.size() / 8 + 4;
        std::vector<uint8_t> out_buf(out_max);
        uint8_t* ptr = out_buf.data() + out_buf.size();

        RansState rans;
        RansEncInit(&rans);
        for (size_t i = data.size(); i > 0; i--) {
            uint8_t s = data[i - 1];
            RansEncPutSymbol(&rans, &ptr, &esyms[s]);
        }
        RansEncFlush(&rans, &ptr);

        uint8_t* rans_begin = ptr;
        size_t rans_size = static_cast<size_t>(out_buf.data() + out_buf.size() - rans_begin);

        std::ofstream out(argv[2], std::ios::binary);
        if (!out) throw std::runtime_error("Cannot create output file");

        out.write("RANS", 4);
        write_u8(out, static_cast<uint8_t>(PROB_BITS));
        write_u64(out, static_cast<uint64_t>(data.size()));
        for (int i = 0; i < 256; i++) {
            if (stats.freqs[i] == 0 || stats.freqs[i] >= (1u << 16))
                throw std::runtime_error("Invalid frequency table");
            write_u16(out, static_cast<uint16_t>(stats.freqs[i]));
        }
        out.write(reinterpret_cast<const char*>(rans_begin), rans_size);

        std::cout << "Compressed to: " << argv[2] << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
