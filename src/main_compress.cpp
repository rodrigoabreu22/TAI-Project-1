/*
 * TAI Project 1 — Compressor (Block BWT + MTF + Context Mixing + Range Coding)
 *
 * Pipeline:
 *   1. Read input into memory.
 *   2. For each 4 MiB block: apply BWT then MTF.
 *   3. Encode each transformed block bit-by-bit with a context-mixing model
 *      and range coder.
 *
 * File format (TA10)
 * ──────────────────
 *  Bytes 0-3    Magic "TA10"
 *  Bytes 4-11   uint64_t original_size  (little-endian)
 *  Bytes 12-15  uint32_t block_size     (little-endian)
 *  Bytes 16-19  uint32_t block_count    (little-endian)
 *  Next         uint32_t primary_index per block (little-endian)
 *  Rest         Range-coded CM bitstream of the BWT+MTF blocks
 *
 * Usage:  compress <input_file> <output_file> [block_size_kib]
 *         compress          (stdin -> stdout)
 */

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "coder/RangeCoder.hpp"
#include "model/BwtTransform.hpp"
#include "model/ContextMixModel.hpp"
#include "model/MoveToFront.hpp"

namespace {

void writeUint64LE(std::ostream &out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.put(static_cast<char>((v >> (i * 8)) & 0xFFu));
}

void writeUint32LE(std::ostream &out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.put(static_cast<char>((v >> (i * 8)) & 0xFFu));
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 1 && argc != 3 && argc != 4) {
        std::cerr << "Usage: compress <input_file> <output_file> [block_size_kib]\n"
                     "       compress          (stdin -> stdout)\n";
        return EXIT_FAILURE;
    }

    // --- Read input ---
    std::ifstream file_in;
    if (argc >= 3) {
        file_in.open(argv[1], std::ios::binary);
        if (!file_in) {
            std::cerr << "Error: cannot open input file: " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::istream &in = (argc >= 3) ? static_cast<std::istream &>(file_in) : std::cin;

    std::vector<std::uint8_t> raw_data;
    for (int b; (b = in.get()) != std::char_traits<char>::eof();)
        raw_data.push_back(static_cast<std::uint8_t>(b));

    // --- Parse optional block size ---
    constexpr std::uint32_t kDefaultBlockSize = 4u * 1024u * 1024u;
    std::uint32_t block_size = kDefaultBlockSize;
    if (argc == 4) {
        const long long kib = std::atoll(argv[3]);
        const unsigned long long bytes = static_cast<unsigned long long>(kib) * 1024ull;
        if (kib <= 0 || bytes == 0 || bytes > 0xFFFFFFFFull) {
            std::cerr << "Error: invalid block_size_kib.\n";
            return EXIT_FAILURE;
        }
        block_size = static_cast<std::uint32_t>(bytes);
    }

    // --- BWT + MTF per block ---
    const std::size_t n_blocks = (raw_data.size() + block_size - 1) / block_size;
    std::vector<std::vector<std::uint8_t>> blocks;
    std::vector<std::uint32_t> primary_indices;
    blocks.reserve(n_blocks);
    primary_indices.reserve(n_blocks);

    for (std::size_t offset = 0; offset < raw_data.size(); offset += block_size) {
        const std::size_t len = std::min<std::size_t>(block_size, raw_data.size() - offset);
        std::vector<std::uint8_t> block(raw_data.begin() + offset,
                                        raw_data.begin() + offset + len);
        auto [bwt_out, primary_index] = bwt_forward(block);
        blocks.push_back(mtf_forward(bwt_out));
        primary_indices.push_back(primary_index);
    }

    // --- Open output and write header ---
    std::ofstream file_out;
    if (argc >= 3) {
        file_out.open(argv[2], std::ios::binary);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << argv[2] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::ostream &out = (argc >= 3) ? static_cast<std::ostream &>(file_out) : std::cout;

    out.write("TA10", 4);
    writeUint64LE(out, static_cast<std::uint64_t>(raw_data.size()));
    writeUint32LE(out, block_size);
    writeUint32LE(out, static_cast<std::uint32_t>(n_blocks));
    for (std::uint32_t pi : primary_indices)
        writeUint32LE(out, pi);

    // --- Range-encode each block ---
    try {
        RangeEncoder encoder(out);
        for (const auto &block : blocks) {
            ContextMixModel model;
            for (std::uint8_t byte : block) {
                for (int bit = 7; bit >= 0; --bit) {
                    const std::uint32_t prob1 = model.predict();
                    BinaryFrequencyTable freqs(prob1);
                    encoder.write(freqs, (byte >> bit) & 1u);
                    model.update((byte >> bit) & 1u);
                }
            }
        }
        encoder.finish();
    } catch (const std::exception &e) {
        std::cerr << "Encoding error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
