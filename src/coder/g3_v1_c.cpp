#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

struct Statistics {
    long long original_size;
    long long compressed_size;
    double compression_ratio;
    long long space_saved;
};

class ArithmeticEncoder {
private:
    static const uint32_t MAX_RANGE = 0xFFFFFFFFu;
    static const uint32_t HALF = 0x80000000u;
    static const uint32_t FIRST_QTR = 0x40000000u;
    static const uint32_t THIRD_QTR = 0xC0000000u;

    struct Symbol {
        unsigned char value;
        uint64_t low;
        uint64_t high;
        uint64_t count;
    };

    std::vector<Symbol> symbols;
    uint64_t total_count = 0;

    void buildFrequencyTable(const std::vector<unsigned char>& data) {
        symbols.clear();
        std::map<unsigned char, long long> freq;
        for (unsigned char byte : data) {
            freq[byte]++;
        }

        total_count = data.size();
        uint64_t cumulative = 0;

        for (auto& [value, count] : freq) {
            symbols.push_back({value, cumulative, cumulative + static_cast<uint64_t>(count), static_cast<uint64_t>(count)});
            cumulative += count;
        }
    }

    struct BitWriter {
        std::vector<unsigned char> bytes;
        uint8_t current = 0;
        int bits_filled = 0;

        void writeBit(int bit) {
            current = static_cast<uint8_t>((current << 1) | (bit & 1));
            bits_filled++;
            if (bits_filled == 8) {
                bytes.push_back(current);
                current = 0;
                bits_filled = 0;
            }
        }

        void flush() {
            if (bits_filled > 0) {
                current <<= (8 - bits_filled);
                bytes.push_back(current);
                current = 0;
                bits_filled = 0;
            }
        }
    };

    struct BitReader {
        const std::vector<unsigned char>& bytes;
        size_t index = 0;
        uint8_t current = 0;
        int bits_left = 0;

        explicit BitReader(const std::vector<unsigned char>& data) : bytes(data) {}

        int readBit() {
            if (bits_left == 0) {
                if (index >= bytes.size()) {
                    return 0;
                }
                current = bytes[index++];
                bits_left = 8;
            }
            int bit = (current >> 7) & 1;
            current <<= 1;
            bits_left--;
            return bit;
        }
    };

    void encodeData(const std::vector<unsigned char>& data, BitWriter& writer) {
        uint32_t low = 0;
        uint32_t high = MAX_RANGE;
        uint32_t bits_to_follow = 0;

        for (unsigned char byte : data) {
            auto it = std::find_if(symbols.begin(), symbols.end(),
                [byte](const Symbol& s) { return s.value == byte; });

            if (it == symbols.end()) continue;

            uint64_t range = static_cast<uint64_t>(high - low) + 1;
            high = static_cast<uint32_t>(low + (range * it->high) / total_count - 1);
            low = static_cast<uint32_t>(low + (range * it->low) / total_count);

            for (;;) {
                if (high < HALF) {
                    writer.writeBit(0);
                    while (bits_to_follow > 0) {
                        writer.writeBit(1);
                        bits_to_follow--;
                    }
                } else if (low >= HALF) {
                    writer.writeBit(1);
                    while (bits_to_follow > 0) {
                        writer.writeBit(0);
                        bits_to_follow--;
                    }
                    low -= HALF;
                    high -= HALF;
                } else if (low >= FIRST_QTR && high < THIRD_QTR) {
                    bits_to_follow++;
                    low -= FIRST_QTR;
                    high -= FIRST_QTR;
                } else {
                    break;
                }
                low <<= 1;
                high = (high << 1) | 1;
            }
        }

        bits_to_follow++;
        if (low < FIRST_QTR) {
            writer.writeBit(0);
            while (bits_to_follow-- > 0) writer.writeBit(1);
        } else {
            writer.writeBit(1);
            while (bits_to_follow-- > 0) writer.writeBit(0);
        }
        writer.flush();
    }

    void buildSymbolsFromCounts(const std::vector<std::pair<unsigned char, uint64_t>>& counts) {
        symbols.clear();
        total_count = 0;
        uint64_t cumulative = 0;
        for (const auto& [value, count] : counts) {
            symbols.push_back({value, cumulative, cumulative + count, count});
            cumulative += count;
        }
        total_count = cumulative;
    }

    unsigned char decodeSymbol(uint64_t cum) const {
        for (const auto& s : symbols) {
            if (cum >= s.low && cum < s.high) {
                return s.value;
            }
        }
        return 0;
    }

    void decodeData(BitReader& reader, std::vector<unsigned char>& output, uint64_t original_size) {
        uint32_t low = 0;
        uint32_t high = MAX_RANGE;
        uint32_t value = 0;
        for (int i = 0; i < 32; i++) {
            value = (value << 1) | reader.readBit();
        }

        for (uint64_t i = 0; i < original_size; i++) {
            uint64_t range = static_cast<uint64_t>(high - low) + 1;
            uint64_t cum = ((static_cast<uint64_t>(value - low) + 1) * total_count - 1) / range;
            unsigned char symbol_value = decodeSymbol(cum);
            output.push_back(symbol_value);

            auto it = std::find_if(symbols.begin(), symbols.end(),
                [symbol_value](const Symbol& s) { return s.value == symbol_value; });
            if (it == symbols.end()) continue;

            high = static_cast<uint32_t>(low + (range * it->high) / total_count - 1);
            low = static_cast<uint32_t>(low + (range * it->low) / total_count);

            for (;;) {
                if (high < HALF) {
                    // do nothing
                } else if (low >= HALF) {
                    low -= HALF;
                    high -= HALF;
                    value -= HALF;
                } else if (low >= FIRST_QTR && high < THIRD_QTR) {
                    low -= FIRST_QTR;
                    high -= FIRST_QTR;
                    value -= FIRST_QTR;
                } else {
                    break;
                }
                low <<= 1;
                high = (high << 1) | 1;
                value = (value << 1) | reader.readBit();
            }
        }
    }

    static void writeUint64(std::ofstream& out, uint64_t v) {
        for (int i = 0; i < 8; i++) {
            out.put(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
        }
    }

    static uint64_t readUint64(std::ifstream& in) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            int c = in.get();
            if (c == EOF) throw std::runtime_error("Unexpected EOF");
            v = (v << 8) | static_cast<uint64_t>(static_cast<unsigned char>(c));
        }
        return v;
    }

    static void writeUint32(std::ofstream& out, uint32_t v) {
        for (int i = 0; i < 4; i++) {
            out.put(static_cast<char>((v >> (24 - 8 * i)) & 0xFF));
        }
    }

    static uint32_t readUint32(std::ifstream& in) {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            int c = in.get();
            if (c == EOF) throw std::runtime_error("Unexpected EOF");
            v = (v << 8) | static_cast<uint32_t>(static_cast<unsigned char>(c));
        }
        return v;
    }

public:
    Statistics compress(const std::string& input_file, const std::string& output_file) {
        std::ifstream infile(input_file, std::ios::binary);
        if (!infile) {
            throw std::runtime_error("Cannot open input file");
        }

        std::vector<unsigned char> data((std::istreambuf_iterator<char>(infile)),
                                        std::istreambuf_iterator<char>());
        infile.close();

        long long original_size = data.size();

        buildFrequencyTable(data);

        BitWriter writer;
        encodeData(data, writer);

        std::ofstream outfile(output_file, std::ios::binary);
        if (!outfile) {
            throw std::runtime_error("Cannot create output file");
        }
        outfile.write("ARIT", 4);
        writeUint64(outfile, static_cast<uint64_t>(original_size));
        writeUint32(outfile, static_cast<uint32_t>(symbols.size()));
        for (const auto& s : symbols) {
            outfile.put(static_cast<char>(s.value));
            writeUint64(outfile, s.count);
        }

        outfile.write(reinterpret_cast<const char*>(writer.bytes.data()), writer.bytes.size());
        outfile.close();

        long long compressed_size = static_cast<long long>(writer.bytes.size()) +
                                    4 + 8 + 4 + static_cast<long long>(symbols.size()) * (1 + 8);

        return {
            original_size,
            compressed_size,
            (double)compressed_size / original_size,
            original_size - compressed_size
        };
    }

    void decompress(const std::string& input_file, const std::string& output_file) {
        std::ifstream infile(input_file, std::ios::binary);
        if (!infile) {
            throw std::runtime_error("Cannot open input file");
        }

        char magic[4];
        infile.read(magic, 4);
        if (infile.gcount() != 4 || std::string(magic, 4) != "ARIT") {
            throw std::runtime_error("Invalid file format");
        }

        uint64_t original_size = readUint64(infile);
        uint32_t symbol_count = readUint32(infile);
        std::vector<std::pair<unsigned char, uint64_t>> counts;
        counts.reserve(symbol_count);
        for (uint32_t i = 0; i < symbol_count; i++) {
            int v = infile.get();
            if (v == EOF) throw std::runtime_error("Unexpected EOF");
            uint64_t count = readUint64(infile);
            counts.emplace_back(static_cast<unsigned char>(v), count);
        }

        std::vector<unsigned char> bitstream((std::istreambuf_iterator<char>(infile)),
                                             std::istreambuf_iterator<char>());
        infile.close();

        buildSymbolsFromCounts(counts);

        BitReader reader(bitstream);
        std::vector<unsigned char> decoded;
        decoded.reserve(static_cast<size_t>(original_size));
        decodeData(reader, decoded, original_size);

        std::ofstream outfile(output_file, std::ios::binary);
        if (!outfile) {
            throw std::runtime_error("Cannot create output file");
        }
        outfile.write(reinterpret_cast<const char*>(decoded.data()), decoded.size());
        outfile.close();
    }

    void printStatistics(const Statistics& stats) {
        std::cout << "\n=== Compression Statistics ===" << std::endl;
        std::cout << "Original size:     " << stats.original_size << " bytes" << std::endl;
        std::cout << "Compressed size:   " << stats.compressed_size << " bytes" << std::endl;
        std::cout << "Compression ratio: " << std::fixed << std::setprecision(4)
                  << stats.compression_ratio * 100 << "%" << std::endl;
        std::cout << "Space saved:       " << stats.space_saved << " bytes ("
                  << std::fixed << std::setprecision(2)
                  << (1 - stats.compression_ratio) * 100 << "%)" << std::endl;
    }
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input_file> <output_file>" << std::endl;
        return 1;
    }

    try {
        ArithmeticEncoder encoder;
        Statistics stats = encoder.compress(argv[1], argv[2]);
        encoder.printStatistics(stats);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
