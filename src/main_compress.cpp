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

#include "coder/ArithmeticCoder.hpp"
#include "coder/BitIoStream.hpp"
#include "model/BwtTransform.hpp"
#include "model/PpmModel.hpp"

static void encodeSymbol(PpmModel &model,
                         const std::deque<std::uint32_t> &history,
                         std::uint32_t symbol,
                         ArithmeticEncoder &enc)
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

    // ── Scan BWT output: build compact alphabet and compute H₀ ────────────────
    bool seen[256] = {};
    long long byte_counts[256] = {};
    long long total_bytes = static_cast<long long>(bwt_data.size());
    for (uint8_t b : bwt_data) {
        seen[b] = true;
        byte_counts[b]++;
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

    // ── Write header (TAI5) ───────────────────────────────────────────────────
    out.write("TAI5", 4);
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

    // ── Encode BWT output via PPM + arithmetic coder ──────────────────────────
    try {
        PpmModel model(MODEL_ORDER, k + 1, k, NODE_LIMIT);
        BitOutputStream bos(out);
        ArithmeticEncoder enc(32, bos);
        std::deque<std::uint32_t> history;

        for (uint8_t b : bwt_data) {
            std::uint32_t sym = encode_map[b];
            encodeSymbol(model, history, sym, enc);
            model.incrementContexts(history, sym);

            if (MODEL_ORDER >= 1) {
                if (history.size() >= static_cast<std::size_t>(MODEL_ORDER))
                    history.pop_back();
                history.push_front(sym);
            }
        }

        encodeSymbol(model, history, k, enc);  // EOF marker = escapeSymbol
        enc.finish();
        bos.finish();

    } catch (const std::exception &e) {
        std::cerr << "Encoding error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
