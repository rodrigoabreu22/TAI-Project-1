/*
 * TAI Project 1 — PPM Arithmetic Decompressor (BWT whole-file + parallel PPM chunks)
 *
 * Pipeline:
 *   1. Read TAI7 global header: primary_index, num_chunks.
 *   2. Read all chunk headers + bitstreams into memory.
 *   3. Decode each chunk in parallel: PPM decode → RLE expand → BWT bytes.
 *   4. Assemble all BWT bytes in order → full bwt_buf.
 *   5. BWT inverse (whole file) → original data.
 *
 * Usage:  decompress <compressed_file> <output_file>
 *         decompress          (stdin → stdout)
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "coder/RangeCoder.hpp"
#include "coder/FrequencyTable.hpp"
#include "model/BwtTransform.hpp"
#include "model/RleTransform.hpp"
#include "model/PpmModel.hpp"


// ── PPM symbol decoder ────────────────────────────────────────────────────────
static std::uint32_t decodeSymbol(RangeDecoder &dec,
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


// ── Per-chunk data ────────────────────────────────────────────────────────────
struct ChunkData {
    int                  model_order;
    uint32_t             k;
    std::vector<uint8_t> alphabet;
    std::vector<uint8_t> bitstream;
};


// ── Decode one chunk → portion of bwt_data (runs in a worker thread) ─────────
static std::vector<uint8_t> decompress_chunk(const ChunkData &cd)
{
    std::size_t NODE_LIMIT;
    {
        std::size_t node_bytes = static_cast<std::size_t>(cd.k + 1) * 12 + 80;
        constexpr std::size_t TARGET = 6ULL * 1024 * 1024 * 1024;
        NODE_LIMIT = std::min(TARGET / node_bytes, std::size_t(8'000'000));
        NODE_LIMIT = std::max(NODE_LIMIT, std::size_t(2'000'000));
    }

    std::string raw(cd.bitstream.begin(), cd.bitstream.end());
    std::istringstream buf(raw, std::ios::binary);

    std::vector<uint8_t> bwt_portion;
    {
        PpmModel model(cd.model_order, cd.k + 1, cd.k, NODE_LIMIT);
        std::vector<std::uint32_t> exp_init(32, 1);
        SimpleFrequencyTable exp_model(exp_init);
        std::vector<std::uint32_t> bit_init(2, 1);
        SimpleFrequencyTable bit_model(bit_init);
        RangeDecoder dec(buf);
        std::deque<std::uint32_t> history;

        while (true) {
            std::uint32_t sym = decodeSymbol(dec, model, history);
            if (sym == cd.k)
                break;  // EOF marker

            model.incrementContexts(history, sym);
            if (cd.model_order >= 1) {
                if (history.size() >= static_cast<std::size_t>(cd.model_order))
                    history.pop_back();
                history.push_front(sym);
            }

            std::uint32_t b = dec.read(exp_model);
            exp_model.increment(b);
            std::uint32_t residual = 0;
            for (std::uint32_t j = 0; j < b; j++)
                residual = (residual << 1) | dec.read(bit_model);
            std::uint32_t cnt = (1u << b) | residual;

            uint8_t byte = cd.alphabet[sym];
            for (std::uint32_t j = 0; j < cnt; j++)
                bwt_portion.push_back(byte);
        }
    }

    return bwt_portion;
}


// ── Read a uint32_t little-endian ─────────────────────────────────────────────
static bool read_u32le(std::istream &in, uint32_t &v) {
    v = 0;
    for (int i = 0; i < 4; i++) {
        int b = in.get();
        if (!in) return false;
        v |= static_cast<uint32_t>(static_cast<uint8_t>(b)) << (i * 8);
    }
    return true;
}


int main(int argc, char *argv[]) {
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: decompress <compressed_file> <output_file>\n"
                     "       decompress          (stdin -> stdout)\n";
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

    // ── Read and validate global header ───────────────────────────────────────
    char magic[4];
    in.read(magic, 4);
    if (!in || magic[0] != 'T' || magic[1] != 'A' || magic[2] != 'I' || magic[3] != '7') {
        std::cerr << "Error: not a TAI7 compressed file.\n";
        return EXIT_FAILURE;
    }

    uint32_t primary_index = 0, num_chunks = 0;
    if (!read_u32le(in, primary_index) || !read_u32le(in, num_chunks)) {
        std::cerr << "Error: truncated global header.\n";
        return EXIT_FAILURE;
    }

    // ── Read all chunk headers + bitstreams ───────────────────────────────────
    std::vector<ChunkData> chunks(num_chunks);
    for (uint32_t i = 0; i < num_chunks; i++) {
        ChunkData &cd = chunks[i];

        uint32_t bitstream_size = 0;
        if (!read_u32le(in, bitstream_size)) {
            std::cerr << "Error: truncated bitstream_size (chunk " << i << ").\n";
            return EXIT_FAILURE;
        }

        int mo = static_cast<int>(static_cast<uint8_t>(in.get()));
        if (!in) {
            std::cerr << "Error: truncated MODEL_ORDER (chunk " << i << ").\n";
            return EXIT_FAILURE;
        }
        cd.model_order = mo;

        uint32_t k_raw = static_cast<uint8_t>(in.get());
        if (!in) {
            std::cerr << "Error: truncated k_raw (chunk " << i << ").\n";
            return EXIT_FAILURE;
        }
        cd.k = (k_raw == 0u) ? 256u : k_raw;

        cd.alphabet.resize(cd.k);
        if (k_raw != 0u) {
            in.read(reinterpret_cast<char *>(cd.alphabet.data()),
                    static_cast<std::streamsize>(cd.k));
            if (!in) {
                std::cerr << "Error: truncated alphabet (chunk " << i << ").\n";
                return EXIT_FAILURE;
            }
        } else {
            for (uint32_t j = 0; j < 256u; j++)
                cd.alphabet[j] = static_cast<uint8_t>(j);
        }

        cd.bitstream.resize(bitstream_size);
        in.read(reinterpret_cast<char *>(cd.bitstream.data()),
                static_cast<std::streamsize>(bitstream_size));
        if (!in) {
            std::cerr << "Error: truncated bitstream (chunk " << i << ").\n";
            return EXIT_FAILURE;
        }
    }

    // ── Decode chunks in parallel ─────────────────────────────────────────────
    std::vector<std::future<std::vector<uint8_t>>> futures;
    futures.reserve(num_chunks);
    for (const auto &cd : chunks)
        futures.push_back(std::async(std::launch::async, decompress_chunk, cd));

    // ── Assemble full bwt_buf from chunk portions in order ────────────────────
    std::vector<uint8_t> bwt_buf;
    for (auto &f : futures) {
        std::vector<uint8_t> portion = f.get();
        bwt_buf.insert(bwt_buf.end(), portion.begin(), portion.end());
    }

    // ── BWT inverse on the whole buffer ───────────────────────────────────────
    std::vector<uint8_t> original = bwt_inverse(bwt_buf, primary_index);

    // ── Open output and write ─────────────────────────────────────────────────
    std::ofstream file_out;
    if (argc == 3) {
        file_out.open(argv[2], std::ios::binary);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << argv[2] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::ostream &out = (argc == 3) ? static_cast<std::ostream &>(file_out) : std::cout;
    out.write(reinterpret_cast<const char *>(original.data()),
              static_cast<std::streamsize>(original.size()));

    return EXIT_SUCCESS;
}
