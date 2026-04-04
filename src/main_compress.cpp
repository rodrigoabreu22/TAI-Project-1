/*
 * TAI Project 1 — MTF+ORDER=0 Compressor (BWT whole-file + parallel MTF chunks)
 *
 * Pipeline:
 *   1. BWT (SA-IS) on the ENTIRE input.
 *   2. RLE of BWT output.
 *   3. Split runs into N chunks (N = hardware thread count).
 *   4. Each chunk: MTF on run symbols → adaptive ORDER=0 range-code → buffer.
 *   5. Write TAI7 header + per-chunk data.
 *
 * MTF (Move-To-Front) converts the run-symbol stream to rank indices
 * heavily concentrated near 0 (BWT clusters identical symbols → same symbol
 * repeats → MTF rank is 0 almost always).  ORDER=0 codes this very cheaply.
 * This is the same principle as bzip2 (BWT + MTF + Huffman).
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
 *  1 byte   uint8_t  MODEL_ORDER  (unused — kept for format compatibility)
 *  1 byte   uint8_t  k_raw  (0 = full 256; 1–255 = actual k)
 *  k bytes  sorted alphabet (omitted if k_raw == 0)
 *  <bitstream_size bytes of range-coded data>
 *
 * Usage:  compress <input_file> <output_file>
 *         compress          (stdin → stdout)
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

#include "coder/RangeCoder.hpp"
#include "coder/FrequencyTable.hpp"
#include "coder/FenwickFrequencyTable.hpp"
#include "model/BwtTransform.hpp"
#include "model/RleTransform.hpp"


// ── Per-chunk result ──────────────────────────────────────────────────────────
struct ChunkResult {
    std::vector<uint8_t> bitstream;
    uint8_t              k_raw;
    std::vector<uint8_t> alphabet;
};


// ── Encode one chunk of runs via MTF + ORDER=0 (runs in a worker thread) ──────
static ChunkResult compress_chunk(
    std::vector<std::pair<uint8_t, uint32_t>> runs)
{
    // ── Build compact alphabet ────────────────────────────────────────────────
    bool seen[256] = {};
    for (auto& [sym, cnt] : runs) seen[sym] = true;

    std::vector<uint8_t>      alphabet;
    std::array<uint32_t, 256> encode_map{};
    for (int i = 0; i < 256; i++) {
        if (seen[i]) {
            encode_map[i] = static_cast<uint32_t>(alphabet.size());
            alphabet.push_back(static_cast<uint8_t>(i));
        }
    }
    uint32_t k = static_cast<uint32_t>(alphabet.size());

    // ── MTF list initialised as [0, 1, ..., k-1] ─────────────────────────────
    std::vector<uint32_t> mtf_list(k);
    std::iota(mtf_list.begin(), mtf_list.end(), 0u);

    // ── Adaptive ORDER=0 model for MTF ranks: symbols 0..k-1 + k as EOF ──────
    FenwickFrequencyTable rank_model(k + 1);
    for (uint32_t i = 0; i <= k; i++) rank_model.increment(i);

    // ── Count models conditioned on rank bucket ───────────────────────────────
    // rank 0 → bucket 0 (dominant symbol, long runs)
    // rank 1 → bucket 1
    // rank 2-3 → bucket 2
    // rank 4+  → bucket 3
    constexpr uint32_t COUNT_CTXS = 4;
    auto rank_ctx = [](uint32_t r) -> uint32_t {
        if (r == 0) return 0;
        if (r == 1) return 1;
        if (r <= 3) return 2;
        return 3;
    };
    std::vector<uint32_t> exp_init(32, 1u);
    std::vector<SimpleFrequencyTable> exp_models(COUNT_CTXS, SimpleFrequencyTable(exp_init));
    std::vector<uint32_t> bit_init(2, 1u);
    std::vector<SimpleFrequencyTable> bit_models(COUNT_CTXS, SimpleFrequencyTable(bit_init));

    std::ostringstream buf(std::ios::binary);
    RangeEncoder enc(buf);

    for (auto& [sym, cnt] : runs) {
        uint32_t s = encode_map[sym];

        // Find rank of s in MTF list (linear scan, k ≤ 256 → fast)
        uint32_t r = 0;
        while (mtf_list[r] != s) r++;

        // Encode rank
        enc.write(rank_model, r);
        rank_model.increment(r);

        // Move s to front: shift elements 0..r-1 right by one
        for (uint32_t i = r; i > 0; i--) mtf_list[i] = mtf_list[i - 1];
        mtf_list[0] = s;

        // Elias-gamma for count, conditioned on rank bucket
        uint32_t ctx = rank_ctx(r);
        int b = 31 - __builtin_clz(cnt);
        enc.write(exp_models[ctx], static_cast<uint32_t>(b));
        exp_models[ctx].increment(static_cast<uint32_t>(b));
        uint32_t residual = cnt - (1u << b);
        for (int i = b - 1; i >= 0; i--)
            enc.write(bit_models[ctx], (residual >> i) & 1u);
    }

    enc.write(rank_model, k);  // EOF marker (rank k is out of [0, k-1])
    enc.finish();

    ChunkResult result;
    std::string s = buf.str();
    result.bitstream.assign(s.begin(), s.end());
    result.k_raw    = (k == 256u) ? 0u : static_cast<uint8_t>(k);
    result.alphabet = (k < 256u) ? alphabet : std::vector<uint8_t>{};
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
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: compress <input_file> <output_file>\n"
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
                                     compress_chunk, chunks[i]));

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
        out.put(0);  // MODEL_ORDER placeholder (unused)
        out.put(static_cast<char>(r.k_raw));
        if (r.k_raw != 0)
            out.write(reinterpret_cast<const char *>(r.alphabet.data()),
                      static_cast<std::streamsize>(r.alphabet.size()));
        out.write(reinterpret_cast<const char *>(r.bitstream.data()),
                  static_cast<std::streamsize>(r.bitstream.size()));
    }

    return EXIT_SUCCESS;
}
