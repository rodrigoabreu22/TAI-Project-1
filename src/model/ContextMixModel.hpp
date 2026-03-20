#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "coder/FrequencyTable.hpp"

class BinaryFrequencyTable final : public FrequencyTable {
public:
    static constexpr std::uint32_t kTotal = 1u << 12;

    explicit BinaryFrequencyTable(std::uint32_t prob1);

    std::uint32_t getSymbolLimit() const override;
    std::uint32_t get(std::uint32_t symbol) const override;
    void set(std::uint32_t symbol, std::uint32_t freq) override;
    void increment(std::uint32_t symbol) override;
    std::uint32_t getTotal() const override;
    std::uint32_t getLow(std::uint32_t symbol) const override;
    std::uint32_t getHigh(std::uint32_t symbol) const override;
    std::uint32_t findSymbol(std::uint32_t value) const override;

private:
    std::uint16_t freq0_;
    std::uint16_t freq1_;
};


class ContextMixModel final {
public:
    ContextMixModel();

    std::uint32_t predict() const;
    void update(std::uint32_t bit);

private:
    struct Entry {
        std::uint32_t key = 0;
        std::uint16_t count0 = 1;
        std::uint16_t count1 = 1;
    };

    class ContextMap {
    public:
        explicit ContextMap(std::size_t size_power_of_two);

        Entry *lookup(std::uint32_t key);

    private:
        std::vector<Entry> table_;
        std::size_t mask_;
    };

    static std::uint32_t hash(std::uint32_t x);
    static std::uint32_t estimate(const Entry &entry);
    static void updateEntry(Entry &entry, std::uint32_t bit);
    std::uint32_t prefixKey() const;
    static constexpr std::size_t kSseBuckets = 32;

    std::array<std::uint16_t, 16> bit0Counts_;
    std::array<std::uint16_t, 16> bit1Counts_;
    std::array<std::array<std::uint16_t, kSseBuckets>, 8> sse0Counts_;
    std::array<std::array<std::uint16_t, kSseBuckets>, 8> sse1Counts_;
    // Second SSE layer conditioned on (prev0 high nibble [0-15], bitPos [0-7], bucket [0-31])
    std::array<std::array<std::array<std::uint16_t, kSseBuckets>, 8>, 16> sse2_0Counts_;
    std::array<std::array<std::array<std::uint16_t, kSseBuckets>, 8>, 16> sse2_1Counts_;
    ContextMap ctx0_;
    ContextMap ctx1_;
    ContextMap ctx2_;
    ContextMap ctx3_;
    ContextMap ctx4_;
    ContextMap runCtx_;

    mutable std::array<Entry *, 6> activeEntries_;
    mutable std::uint8_t lastSseBucket_;
    mutable std::uint8_t lastSse2Bucket_;
    mutable std::uint32_t lastMixedProb_;
    // Adaptive weights for 7 sub-models (stationary + ctx0..ctx4 + runCtx)
    // Stored as scaled integers (fixed-point, multiplied by kWeightScale)
    static constexpr std::int32_t kWeightScale = 1 << 16;
    mutable std::array<std::int32_t, 7> weights_;
    mutable std::array<std::uint32_t, 7> lastProbs_;  // per-model prediction saved for weight update

    std::uint8_t prev0_;
    std::uint8_t prev1_;
    std::uint8_t prev2_;
    std::uint8_t prev3_;
    std::uint8_t currentByte_;
    std::uint8_t bitPos_;
    std::uint16_t runLength_;
};
