/*
 * TAI Project 1 — Ratio-Focused Decompressor (Block BWT + Context Mixing)
 *
 * Reads a TA10 file and reconstructs the original data exactly.
 *
 * Usage:  decompress_ratio <compressed_file> <output_file>
 *         decompress_ratio          (stdin -> stdout)
 */

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

#include "coder/RangeCoder.hpp"
#include "model/BwtTransform.hpp"
#include "model/ContextMixModel.hpp"
#include "model/MoveToFront.hpp"

namespace {

bool readUint64LE(std::istream &in, std::uint64_t &value) {
    value = 0;
    for (int i = 0; i < 8; ++i) {
        int b = in.get();
        if (!in)
            return false;
        value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b)) << (i * 8);
    }
    return true;
}

bool readUint32LE(std::istream &in, std::uint32_t &value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
        int b = in.get();
        if (!in)
            return false;
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << (i * 8);
    }
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    if (argc != 1 && argc != 3) {
        std::cerr << "Usage: decompress_ratio <compressed_file> <output_file>\n"
                     "       decompress_ratio          (stdin -> stdout)\n";
        return EXIT_FAILURE;
    }

    std::ifstream file_in;
    if (argc == 3) {
        file_in.open(argv[1], std::ios::binary);
        if (!file_in) {
            std::cerr << "Error: cannot open input file: " << argv[1] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::istream &in = (argc == 3) ? static_cast<std::istream &>(file_in) : std::cin;

    char magic[4];
    in.read(magic, 4);
    if (!in || magic[0] != 'T' || magic[1] != 'A' || magic[2] != '1' || magic[3] != '0') {
        std::cerr << "Error: not a TA10 compressed file.\n";
        return EXIT_FAILURE;
    }

    std::uint64_t original_size = 0;
    std::uint32_t block_size = 0;
    std::uint32_t block_count = 0;
    if (!readUint64LE(in, original_size) ||
        !readUint32LE(in, block_size) ||
        !readUint32LE(in, block_count)) {
        std::cerr << "Error: truncated header.\n";
        return EXIT_FAILURE;
    }
    if (original_size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        std::cerr << "Error: input too large for this platform.\n";
        return EXIT_FAILURE;
    }
    if (block_size == 0) {
        std::cerr << "Error: invalid block size in header.\n";
        return EXIT_FAILURE;
    }

    const std::uint64_t expected_blocks =
        original_size == 0 ? 0u : (original_size + block_size - 1) / block_size;
    if (block_count != expected_blocks) {
        std::cerr << "Error: inconsistent block count in header.\n";
        return EXIT_FAILURE;
    }

    std::vector<std::uint32_t> primary_indices(block_count, 0);
    for (std::uint32_t i = 0; i < block_count; ++i) {
        if (!readUint32LE(in, primary_indices[i])) {
            std::cerr << "Error: truncated primary indices in header.\n";
            return EXIT_FAILURE;
        }
        const std::uint64_t block_len =
            std::min<std::uint64_t>(block_size, original_size - static_cast<std::uint64_t>(i) * block_size);
        if (block_len > 0 && primary_indices[i] >= block_len) {
            std::cerr << "Error: invalid primary index in header.\n";
            return EXIT_FAILURE;
        }
    }

    std::ofstream file_out;
    if (argc == 3) {
        file_out.open(argv[2], std::ios::binary);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << argv[2] << "\n";
            return EXIT_FAILURE;
        }
    }
    std::ostream &out = (argc == 3) ? static_cast<std::ostream &>(file_out) : std::cout;

    try {
        RangeDecoder decoder(in);
        std::uint64_t remaining = original_size;

        for (std::uint32_t block = 0; block < block_count; ++block) {
            const std::size_t block_len =
                static_cast<std::size_t>(std::min<std::uint64_t>(block_size, remaining));
            ContextMixModel model;
            std::vector<std::uint8_t> bwt_block(block_len, 0);

            for (std::size_t i = 0; i < block_len; ++i) {
                std::uint8_t byte = 0;
                for (int bit = 7; bit >= 0; --bit) {
                    std::uint32_t prob1 = model.predict();
                    BinaryFrequencyTable freqs(prob1);
                    std::uint32_t decoded = decoder.read(freqs);
                    byte = static_cast<std::uint8_t>((byte << 1) | decoded);
                    model.update(decoded);
                }
                bwt_block[i] = byte;
            }

            std::vector<std::uint8_t> original = bwt_inverse(mtf_inverse(bwt_block), primary_indices[block]);
            out.write(reinterpret_cast<const char *>(original.data()),
                      static_cast<std::streamsize>(original.size()));
            remaining -= block_len;
        }
    } catch (const std::exception &e) {
        std::cerr << "Decoding error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
