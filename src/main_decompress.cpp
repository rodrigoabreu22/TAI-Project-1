/*
 * TAI Project 1 — PPM Arithmetic Decompressor
 *
 * Reads the TAI3 format produced by compress and reconstructs the original
 * file exactly (lossless). The PPM model is rebuilt adaptively from the
 * bitstream — no frequency table is stored in the compressed file.
 *
 * Usage:  decompress <compressed_file> <output_file>
 *         decompress          (stdin → stdout, for pipes/benchmark)
 */

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


// Decode one symbol (compact index) using the PPM cascade.
// Mirrors encodeSymbol in main_compress.cpp exactly — both sides must agree.
static std::uint32_t decodeSymbol(ArithmeticDecoder &dec,
                                  PpmModel &model,
                                  const std::deque<std::uint32_t> &history)
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

        {
            std::uint32_t sym = dec.read(ctx->frequencies);
            if (sym != model.escapeSymbol)
                return sym;
        }

        nextOrder:;
    }

    return dec.read(model.orderMinus1Freqs);
}


int main(int argc, char *argv[]) {
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: decompress <compressed_file> <output_file>\n"
                     "       decompress          (stdin -> stdout)\n";
        return EXIT_FAILURE;
    }

    // ── Open streams ──────────────────────────────────────────────────────────
    std::ifstream file_in;
    if (argc == 3) {
        file_in.open(argv[1], std::ios::binary);
        if (!file_in) {
            std::cerr << "Error: cannot open input file: " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::istream &in = (argc == 3) ? static_cast<std::istream &>(file_in) : std::cin;

    // ── Read and validate header ──────────────────────────────────────────────
    char magic[4];
    in.read(magic, 4);
    if (!in || magic[0] != 'T' || magic[1] != 'A' || magic[2] != 'I' || magic[3] != '3') {
        std::cerr << "Error: not a TAI3 compressed file.\n";
        return EXIT_FAILURE;
    }

    int model_order = static_cast<int>(static_cast<uint8_t>(in.get()));
    if (!in || model_order < -1 || model_order > 16) {
        std::cerr << "Error: invalid model order in header.\n";
        return EXIT_FAILURE;
    }

    // k_raw == 0 means full 256-byte alphabet (identity mapping)
    std::uint32_t k_raw = static_cast<std::uint8_t>(in.get());
    if (!in) {
        std::cerr << "Error: truncated header.\n";
        return EXIT_FAILURE;
    }
    std::uint32_t k = (k_raw == 0u) ? 256u : k_raw;

    std::vector<std::uint8_t> alphabet(k);
    if (k < 256u) {
        in.read(reinterpret_cast<char *>(alphabet.data()),
                static_cast<std::streamsize>(k));
        if (!in) {
            std::cerr << "Error: truncated alphabet in header.\n";
            return EXIT_FAILURE;
        }
    } else {
        for (std::uint32_t i = 0; i < 256u; i++)
            alphabet[i] = static_cast<std::uint8_t>(i);
    }

    // NODE_LIMIT must use the same formula as the compressor (same k → same limit).
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

    // ── Decode ────────────────────────────────────────────────────────────────
    try {
        PpmModel model(model_order, k + 1, k, NODE_LIMIT);
        BitInputStream bis(in);
        ArithmeticDecoder dec(32, bis);
        std::deque<std::uint32_t> history;

        while (true) {
            std::uint32_t sym = decodeSymbol(dec, model, history);
            if (sym == k)
                break;  // EOF marker

            out.put(static_cast<char>(alphabet[sym]));
            model.incrementContexts(history, sym);

            if (model_order >= 1) {
                if (history.size() >= static_cast<std::size_t>(model_order))
                    history.pop_back();
                history.push_front(sym);
            }
        }

    } catch (const std::exception &e) {
        std::cerr << "Decoding error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
