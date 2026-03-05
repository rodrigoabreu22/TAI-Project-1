/*
 * TAI Project 1 — PPM Arithmetic Compressor
 *
 * Model:  Prediction by Partial Matching (PPM), adaptive, order MODEL_ORDER.
 *         Based on Nayuki Reference PPM (MIT License).
 *         https://www.nayuki.io/page/reference-arithmetic-coding
 *
 * Coder:  Nayuki Reference Arithmetic Coding (MIT).
 *
 * Compressed file format (TAI4)
 * ─────────────────────────────
 *  Bytes 0-3   Magic "TAI4"
 *  Byte  4     uint8_t MODEL_ORDER
 *  Byte  5     uint8_t k_raw  (0 = full 256-byte alphabet; 1-255 = k distinct bytes)
 *  Bytes 6..   k distinct byte values in sorted order  (omitted when k_raw == 0)
 *  Rest        Arithmetic-coded PPM bitstream using compact symbol indices 0..k-1,
 *              where symbol k = escape (non-negative orders) / EOF (order -1).
 *              No frequency table in header — model is adaptive.
 *
 * Usage:  compress <input_file> <output_file>
 *         compress          (stdin → stdout, adaptive alphabet disabled)
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
#include "model/PpmModel.hpp"

// MODEL_ORDER and NODE_LIMIT are computed at runtime based on alphabet size k.
// Smaller alphabets allow deeper orders and more nodes within the memory budget.

// Encode one symbol (compact index) using the PPM cascade.
// MODEL_ORDER is passed via model.modelOrder.
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
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: compress <input_file> <output_file>\n"
                     "       compress          (stdin -> stdout)\n";
        return EXIT_FAILURE;
    }

    // ── Open input ────────────────────────────────────────────────────────────
    std::ifstream file_in;
    if (argc == 3) {
        file_in.open(argv[1], std::ios::binary);
        if (!file_in) {
            std::cerr << "Error: cannot open input file: " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::istream &in = (argc == 3) ? static_cast<std::istream &>(file_in) : std::cin;

    // ── Build compact alphabet & compute H₀ entropy ───────────────────────────
    // File mode: scan file to find distinct bytes and count frequencies.
    // Stdin mode: assume full 256-byte alphabet (can't seek).
    bool seen[256] = {};
    long long byte_counts[256] = {};
    long long total_bytes = 0;
    if (argc == 3) {
        while (true) {
            int b = in.get();
            if (b == std::char_traits<char>::eof()) break;
            std::uint8_t u = static_cast<std::uint8_t>(b);
            seen[u] = true;
            byte_counts[u]++;
            total_bytes++;
        }
        file_in.clear();                   // reset eofbit before seeking
        file_in.seekg(0, std::ios::beg);
    } else {
        for (int i = 0; i < 256; i++) seen[i] = true;
    }

    // H₀ = Shannon entropy of the raw byte distribution (bits/byte).
    // Only available in file mode (stdin: unknown, treated as 0.0).
    double h0 = 0.0;
    if (argc == 3 && total_bytes > 0) {
        for (int i = 0; i < 256; i++) {
            if (byte_counts[i] > 0) {
                double p = static_cast<double>(byte_counts[i])
                         / static_cast<double>(total_bytes);
                h0 -= p * std::log2(p);
            }
        }
    }

    std::vector<std::uint8_t> alphabet;
    std::array<std::uint32_t, 256> encode_map{};
    for (int i = 0; i < 256; i++) {
        if (seen[i]) {
            encode_map[i] = static_cast<std::uint32_t>(alphabet.size());
            alphabet.push_back(static_cast<std::uint8_t>(i));
        }
    }
    std::uint32_t k = static_cast<std::uint32_t>(alphabet.size());  // 1..256

    // Adaptive MODEL_ORDER: choose based on H₀ entropy first, then alphabet size.
    //   H₀ > 7.9 → near-random data; order-0 adaptive model gives ≈H₀ bits/byte
    //              with no escape-chain overhead (much better than order-5's ~10 b/B).
    //   H₀ > 7.5 → high-entropy data; limit order to 2 to cap escape overhead.
    //   otherwise → smaller alphabet allows deeper order for better compression.
    int MODEL_ORDER;
    if      (argc == 3 && h0 > 7.9) MODEL_ORDER = 0;  // near-random (e.g. /dev/urandom)
    else if (argc == 3 && h0 > 6.8) MODEL_ORDER = 1;  // high-entropy binary (e.g. lightly-structured binary)
    else if (k <= 24)                MODEL_ORDER = 8;
    else if (k <= 48)                MODEL_ORDER = 7;
    else if (k <= 128)               MODEL_ORDER = 6;
    else                             MODEL_ORDER = 5;

    // NODE_LIMIT: target ≤6 GB model memory, floor at 2M to preserve compression quality.
    // Each non-leaf node costs ~(k+1)*12 + 80 bytes (Fenwick tree + subcontexts vector).
    std::size_t NODE_LIMIT;
    {
        std::size_t node_bytes = static_cast<std::size_t>(k + 1) * 12 + 80;
        constexpr std::size_t TARGET = 6ULL * 1024 * 1024 * 1024;
        NODE_LIMIT = std::min(TARGET / node_bytes, std::size_t(8'000'000));
        NODE_LIMIT = std::max(NODE_LIMIT, std::size_t(2'000'000));
    }

    // ── Open output ───────────────────────────────────────────────────────────
    std::ofstream file_out;
    if (argc == 3) {
        file_out.open(argv[2], std::ios::binary);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << argv[2] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::ostream &out = (argc == 3) ? static_cast<std::ostream &>(file_out) : std::cout;

    // ── Write header ──────────────────────────────────────────────────────────
    out.write("TAI4", 4);
    out.put(static_cast<char>(static_cast<uint8_t>(MODEL_ORDER)));

    // k_raw: 0 means 256 (full alphabet, no bytes written); 1-255 = k < 256
    std::uint8_t k_raw = (k == 256u) ? 0u : static_cast<std::uint8_t>(k);
    out.put(static_cast<char>(k_raw));
    if (k < 256u)
        out.write(reinterpret_cast<const char *>(alphabet.data()),
                  static_cast<std::streamsize>(k));

    // ── Encode ────────────────────────────────────────────────────────────────
    // symbolLimit = k+1: compact indices 0..k-1 + symbol k (escape/EOF)
    // escapeSymbol = k
    try {
        PpmModel model(MODEL_ORDER, k + 1, k, NODE_LIMIT);
        BitOutputStream bos(out);
        ArithmeticEncoder enc(32, bos);
        std::deque<std::uint32_t> history;

        while (true) {
            int b = in.get();
            if (b == std::char_traits<char>::eof())
                break;

            std::uint32_t sym = encode_map[static_cast<std::uint8_t>(b)];
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
