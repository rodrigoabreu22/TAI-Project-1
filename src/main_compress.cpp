/*
 * TAI Project 1 — PPM Arithmetic Compressor (BWT + PPM)
 *
 * Pipeline:
 *   1. Read entire input into memory.
 *   2. Apply BWT (Burrows-Wheeler Transform) forward pass.
 *      BWT clusters characters with similar right-contexts together,
 *      so a low-order PPM on the BWT output is equivalent to a much
 *      higher-order PPM on the raw input.
 *   3. Scan BWT output to discover compact alphabet and compute H₀.
 *   4. Choose MODEL_ORDER adaptively (lower than pre-BWT because BWT
 *      already captures long-range structure).
 *   5. Encode BWT output with PPM + arithmetic coding (Nayuki, MIT).
 *
 * Compressed file format (TAI5)
 * ─────────────────────────────
 *  Bytes 0-3   Magic "TAI5"
 *  Byte  4     uint8_t  MODEL_ORDER
 *  Byte  5     uint8_t  k_raw  (0 = full 256-byte alphabet; 1-255 = k)
 *  Bytes 6..   k distinct byte values in sorted order (omitted if k_raw==0)
 *  Next  4     uint32_t primary_index (little-endian) — needed for BWT inverse
 *  Rest        Arithmetic-coded PPM bitstream (adaptive, no freq table stored)
 *
 * Usage:  compress <input_file> <output_file>
 *         compress          (stdin → stdout)
 */

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "coder/RangeCoder.hpp"
#include "model/BwtTransform.hpp"
#include "model/RleTransform.hpp"
#include "model/PpmModel.hpp"

static void encodeSymbol(PpmModel &model,
                         const std::deque<std::uint32_t> &history,
                         std::uint32_t symbol,
                         RangeEncoder &enc)
{
    for (int order = static_cast<int>(history.size()); order >= 0; order--) {
        PpmModel::Context *ctx = model.rootContext.get();

        for (int i = 0; i < order; i++) {
            if (!ctx->hasSubctx)
                throw std::logic_error("Assertion error");
            ctx = ctx->subcontexts.at(history.at(static_cast<std::size_t>(i))).get();
            if (ctx == nullptr)
                goto nextOrder;
        }

        if (symbol != model.escapeSymbol && ctx->frequencies.get(symbol) > 0) {
            enc.write(ctx->frequencies, symbol);
            return;
        }
        enc.write(ctx->frequencies, model.escapeSymbol);

        nextOrder:;
    }

    enc.write(model.orderMinus1Freqs, symbol);
}


int main(int argc, char *argv[]) {
    if (argc != 1 && argc != 3 && argc != 4) {
        std::cerr << "Usage: compress <input_file> <output_file> [order_override]\n"
                     "       compress          (stdin -> stdout)\n";
        return EXIT_FAILURE;
    }

    // ── Open input ────────────────────────────────────────────────────────────
    std::ifstream file_in;
    if (argc >= 3) {
        file_in.open(argv[1], std::ios::binary);
        if (!file_in) {
            std::cerr << "Error: cannot open input file: " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::istream &in = (argc >= 3) ? static_cast<std::istream &>(file_in) : std::cin;

    // ── Read entire input into memory ─────────────────────────────────────────
    std::vector<uint8_t> raw_data;
    {
        int b;
        while ((b = in.get()) != std::char_traits<char>::eof())
            raw_data.push_back(static_cast<uint8_t>(b));
    }

    // ── BWT forward transform ─────────────────────────────────────────────────
    auto [bwt_data, primary_index] = bwt_forward(raw_data);
    raw_data.clear();
    raw_data.shrink_to_fit();

    // ── RLE of BWT output ─────────────────────────────────────────────────────
    auto runs = rle_encode(bwt_data);
    bwt_data.clear();
    bwt_data.shrink_to_fit();

    // ── Scan BWT byte distribution (from runs): build compact alphabet and H₀ ─
    bool seen[256] = {};
    long long byte_counts[256] = {};
    long long total_bytes = 0;
    for (auto& [sym, cnt] : runs) {
        seen[sym] = true;
        byte_counts[sym] += static_cast<long long>(cnt);
        total_bytes     += static_cast<long long>(cnt);
    }

    double h0 = 0.0;
    if (total_bytes > 0) {
        for (int i = 0; i < 256; i++) {
            if (byte_counts[i] > 0) {
                double p = static_cast<double>(byte_counts[i]) / static_cast<double>(total_bytes);
                h0 -= p * std::log2(p);
            }
        }
    }

    std::vector<uint8_t> alphabet;
    std::array<uint32_t, 256> encode_map{};
    for (int i = 0; i < 256; i++) {
        if (seen[i]) {
            encode_map[i] = static_cast<uint32_t>(alphabet.size());
            alphabet.push_back(static_cast<uint8_t>(i));
        }
    }
    uint32_t k = static_cast<uint32_t>(alphabet.size());  // 1..256

    // ── Adaptive MODEL_ORDER ──────────────────────────────────────────────────
    // BWT already captures long-range context, so we need much less PPM order.
    // H₀ is computed from the BWT output (typically lower than raw input H₀).
    int MODEL_ORDER;
    if (argc == 4) {
        MODEL_ORDER = std::atoi(argv[3]);
    } else if (h0 > 7.9)              MODEL_ORDER = 0;  // near-random — BWT useless
    else if (h0 > 6.5)              MODEL_ORDER = 1;  // high-entropy: BWT runs need only ORDER=1
    else if (k <= 200 && h0 > 4.8)  MODEL_ORDER = 2;  // medium k + medium H₀: ORDER=2 helps
    else                             MODEL_ORDER = 1;  // large k or low H₀: ORDER=1 optimal


    // ── NODE_LIMIT ────────────────────────────────────────────────────────────
    std::size_t NODE_LIMIT;
    {
        std::size_t node_bytes = static_cast<std::size_t>(k + 1) * 12 + 80;
        constexpr std::size_t TARGET = 6ULL * 1024 * 1024 * 1024;
        NODE_LIMIT = std::min(TARGET / node_bytes, std::size_t(8'000'000));
        NODE_LIMIT = std::max(NODE_LIMIT, std::size_t(2'000'000));
    }

    // ── Open output ───────────────────────────────────────────────────────────
    std::ofstream file_out;
    if (argc >= 3) {
        file_out.open(argv[2], std::ios::binary);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << argv[2] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::ostream &out = (argc >= 3) ? static_cast<std::ostream &>(file_out) : std::cout;

    // ── Write header (TAI6) ───────────────────────────────────────────────────
    out.write("TAI6", 4);
    out.put(static_cast<char>(static_cast<uint8_t>(MODEL_ORDER)));

    uint8_t k_raw = (k == 256u) ? 0u : static_cast<uint8_t>(k);
    out.put(static_cast<char>(k_raw));
    if (k < 256u)
        out.write(reinterpret_cast<const char *>(alphabet.data()),
                  static_cast<std::streamsize>(k));

    // primary_index: 4 bytes little-endian
    uint32_t pi = primary_index;
    out.put(static_cast<char>((pi >>  0) & 0xFF));
    out.put(static_cast<char>((pi >>  8) & 0xFF));
    out.put(static_cast<char>((pi >> 16) & 0xFF));
    out.put(static_cast<char>((pi >> 24) & 0xFF));

    // ── Encode run symbols + counts in one interleaved arithmetic stream ────────
    // Per run: encode symbol via PPM, then encode count via Elias-gamma.
    //   count n ≥ 1:  b = floor(log2(n)) via adaptive 32-symbol model,
    //                 then b residual bits via flat 2-symbol model.
    try {
        PpmModel model(MODEL_ORDER, k + 1, k, NODE_LIMIT);
        std::vector<std::uint32_t> exp_init(32, 1);
        SimpleFrequencyTable exp_model(exp_init);
        std::vector<std::uint32_t> bit_init(2, 1);
        SimpleFrequencyTable bit_model(bit_init);
        RangeEncoder enc(out);
        std::deque<std::uint32_t> history;

        for (auto& [sym, cnt] : runs) {
            std::uint32_t s = encode_map[sym];
            encodeSymbol(model, history, s, enc);
            model.incrementContexts(history, s);

            if (MODEL_ORDER >= 1) {
                if (history.size() >= static_cast<std::size_t>(MODEL_ORDER))
                    history.pop_back();
                history.push_front(s);
            }

            // Elias-gamma for count
            int b = 31 - __builtin_clz(cnt);  // floor(log2(cnt)); safe since cnt >= 1
            enc.write(exp_model, static_cast<std::uint32_t>(b));
            exp_model.increment(static_cast<std::uint32_t>(b));
            std::uint32_t residual = cnt - (1u << b);
            for (int i = b - 1; i >= 0; i--)
                enc.write(bit_model, (residual >> i) & 1u);
        }

        encodeSymbol(model, history, k, enc);  // EOF marker
        enc.finish();

    } catch (const std::exception &e) {
        std::cerr << "Encoding error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
