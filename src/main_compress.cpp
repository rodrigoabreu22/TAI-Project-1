/*
 * TAI Project 1 — PPM Arithmetic Compressor (BWT whole-file + parallel PPM chunks)
 *
 * Pipeline:
 *   1. BWT (SA-IS) on the ENTIRE input — preserves full clustering benefit.
 *   2. RLE of BWT output.
 *   3. Split runs into N chunks (N = hardware thread count).
 *   4. Each chunk: pre-scan → PPM + range-code → buffer.  All chunks in parallel.
 *   5. Write TAI7 header + per-chunk data.
 *
 * Only the PPM encoding phase is parallelized; BWT runs once on the whole file.
 * This preserves almost all of the compression ratio of TAI6 while cutting the
 * PPM encoding time (the dominant cost) by ~N×.
 *
 * Compressed file format (TAI7)
 * ─────────────────────────────
 *  [Global header]
 *  Bytes 0–3   Magic "TAI7"
 *  Bytes 4–7   uint32_t primary_index (LE) — for whole-file BWT inverse
 *  Bytes 8–11  uint32_t num_chunks (LE)
 *
 *  [Per chunk, repeated num_chunks times]
 *  4 bytes  uint32_t bitstream_size (LE)
 *  1 byte   uint8_t  MODEL_ORDER
 *  1 byte   uint8_t  k_raw  (0 = full 256; 1–255 = actual k)
 *  k bytes  sorted alphabet (omitted if k_raw == 0)
 *  <bitstream_size bytes of range-coded data>
 *
 * Usage:  compress <input_file> <output_file> [order_override]
 *         compress          (stdin → stdout, single chunk)
 */

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "coder/RangeCoder.hpp"
#include "model/BwtTransform.hpp"
#include "model/RleTransform.hpp"
#include "model/PpmModel.hpp"

// ── PPM symbol encoder ────────────────────────────────────────────────────────
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


// ── Per-chunk result ──────────────────────────────────────────────────────────
struct ChunkResult {
    std::vector<uint8_t> bitstream;
    uint8_t              model_order;
    uint8_t              k_raw;
    std::vector<uint8_t> alphabet;    // k sorted byte values (empty if k == 256)
};


// ── Encode one chunk of runs (runs in a worker thread) ────────────────────────
static ChunkResult compress_chunk(
    std::vector<std::pair<uint8_t, uint32_t>> runs,
    int order_override)
{
    // ── Pre-scan ──────────────────────────────────────────────────────────────
    bool      seen[256]        = {};
    long long byte_counts[256] = {};
    long long total_bytes      = 0;
    for (auto& [sym, cnt] : runs) {
        seen[sym]        = true;
        byte_counts[sym] += static_cast<long long>(cnt);
        total_bytes      += static_cast<long long>(cnt);
    }

    double h0 = 0.0;
    if (total_bytes > 0) {
        for (int i = 0; i < 256; i++) {
            if (byte_counts[i] > 0) {
                double p = static_cast<double>(byte_counts[i])
                         / static_cast<double>(total_bytes);
                h0 -= p * std::log2(p);
            }
        }
    }

    std::vector<uint8_t>      alphabet;
    std::array<uint32_t, 256> encode_map{};
    for (int i = 0; i < 256; i++) {
        if (seen[i]) {
            encode_map[i] = static_cast<uint32_t>(alphabet.size());
            alphabet.push_back(static_cast<uint8_t>(i));
        }
    }
    uint32_t k = static_cast<uint32_t>(alphabet.size());

    int MODEL_ORDER;
    if (order_override >= 0) {
        MODEL_ORDER = order_override;
    } else if (h0 > 7.9)             MODEL_ORDER = 0;
    else if (h0 > 6.5)               MODEL_ORDER = 1;
    else if (k <= 200 && h0 > 4.8)   MODEL_ORDER = 2;
    else                              MODEL_ORDER = 1;

    std::size_t NODE_LIMIT;
    {
        std::size_t node_bytes = static_cast<std::size_t>(k + 1) * 12 + 80;
        constexpr std::size_t TARGET = 6ULL * 1024 * 1024 * 1024;
        NODE_LIMIT = std::min(TARGET / node_bytes, std::size_t(8'000'000));
        NODE_LIMIT = std::max(NODE_LIMIT, std::size_t(2'000'000));
    }

    // ── Encode into an in-memory buffer ───────────────────────────────────────
    std::ostringstream buf(std::ios::binary);
    {
        PpmModel model(MODEL_ORDER, k + 1, k, NODE_LIMIT);
        std::vector<std::uint32_t> exp_init(32, 1);
        SimpleFrequencyTable exp_model(exp_init);
        std::vector<std::uint32_t> bit_init(2, 1);
        SimpleFrequencyTable bit_model(bit_init);
        RangeEncoder enc(buf);
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

            int b = 31 - __builtin_clz(cnt);
            enc.write(exp_model, static_cast<std::uint32_t>(b));
            exp_model.increment(static_cast<std::uint32_t>(b));
            std::uint32_t residual = cnt - (1u << b);
            for (int i = b - 1; i >= 0; i--)
                enc.write(bit_model, (residual >> i) & 1u);
        }

        encodeSymbol(model, history, k, enc);  // EOF marker
        enc.finish();
    }

    ChunkResult result;
    std::string s = buf.str();
    result.bitstream.assign(s.begin(), s.end());
    result.model_order = static_cast<uint8_t>(MODEL_ORDER);
    result.k_raw       = (k == 256u) ? 0u : static_cast<uint8_t>(k);
    result.alphabet    = (k < 256u) ? alphabet : std::vector<uint8_t>{};
    return result;
}


// ── Write a uint32_t little-endian ───────────────────────────────────────────
static void write_u32le(std::ostream &out, uint32_t v) {
    out.put(static_cast<char>((v >>  0) & 0xFF));
    out.put(static_cast<char>((v >>  8) & 0xFF));
    out.put(static_cast<char>((v >> 16) & 0xFF));
    out.put(static_cast<char>((v >> 24) & 0xFF));
}


int main(int argc, char *argv[]) {
    if (argc != 1 && argc != 3 && argc != 4) {
        std::cerr << "Usage: compress <input_file> <output_file> [order_override]\n"
                     "       compress          (stdin -> stdout)\n";
        return EXIT_FAILURE;
    }

    int order_override = -1;
    if (argc == 4)
        order_override = std::atoi(argv[3]);

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

    // ── Read entire input ─────────────────────────────────────────────────────
    std::vector<uint8_t> raw_data;
    {
        int b;
        while ((b = in.get()) != std::char_traits<char>::eof())
            raw_data.push_back(static_cast<uint8_t>(b));
    }

    // ── BWT forward on the whole file ─────────────────────────────────────────
    auto [bwt_data, primary_index] = bwt_forward(raw_data);
    raw_data.clear();
    raw_data.shrink_to_fit();

    // ── RLE of BWT output ─────────────────────────────────────────────────────
    auto runs = rle_encode(bwt_data);
    bwt_data.clear();
    bwt_data.shrink_to_fit();

    // ── Split runs into chunks (one per hardware thread) ─────────────────────
    uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
    uint32_t num_chunks = static_cast<uint32_t>(
        std::min(static_cast<size_t>(hw), runs.size()));
    if (num_chunks == 0) num_chunks = 1;

    // Distribute runs as evenly as possible across chunks
    size_t runs_per_chunk = (runs.size() + num_chunks - 1) / num_chunks;
    std::vector<std::vector<std::pair<uint8_t, uint32_t>>> chunks(num_chunks);
    for (uint32_t i = 0; i < num_chunks; i++) {
        size_t start = i * runs_per_chunk;
        size_t end   = std::min(start + runs_per_chunk, runs.size());
        if (start >= runs.size()) { num_chunks = i; break; }
        chunks[i].assign(runs.begin() + static_cast<std::ptrdiff_t>(start),
                         runs.begin() + static_cast<std::ptrdiff_t>(end));
    }
    runs.clear();
    runs.shrink_to_fit();

    // ── Encode chunks in parallel ─────────────────────────────────────────────
    std::vector<std::future<ChunkResult>> futures;
    futures.reserve(num_chunks);
    for (uint32_t i = 0; i < num_chunks; i++)
        futures.push_back(std::async(std::launch::async,
                                     compress_chunk, chunks[i], order_override));

    std::vector<ChunkResult> results;
    results.reserve(num_chunks);
    for (auto &f : futures)
        results.push_back(f.get());

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

    // ── Write TAI7 global header ──────────────────────────────────────────────
    out.write("TAI7", 4);
    write_u32le(out, primary_index);
    write_u32le(out, num_chunks);

    // ── Write each chunk ──────────────────────────────────────────────────────
    for (const auto &r : results) {
        write_u32le(out, static_cast<uint32_t>(r.bitstream.size()));
        out.put(static_cast<char>(r.model_order));
        out.put(static_cast<char>(r.k_raw));
        if (r.k_raw != 0)
            out.write(reinterpret_cast<const char *>(r.alphabet.data()),
                      static_cast<std::streamsize>(r.alphabet.size()));
        out.write(reinterpret_cast<const char *>(r.bitstream.data()),
                  static_cast<std::streamsize>(r.bitstream.size()));
    }

    return EXIT_SUCCESS;
}
