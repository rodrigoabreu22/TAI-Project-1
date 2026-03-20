#include "model/ContextMixModel.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

BinaryFrequencyTable::BinaryFrequencyTable(std::uint32_t prob1) {
    if (prob1 == 0)
        prob1 = 1;
    if (prob1 >= kTotal)
        prob1 = kTotal - 1;
    freq1_ = static_cast<std::uint16_t>(prob1);
    freq0_ = static_cast<std::uint16_t>(kTotal - prob1);
}

std::uint32_t BinaryFrequencyTable::getSymbolLimit() const {
    return 2;
}

std::uint32_t BinaryFrequencyTable::get(std::uint32_t symbol) const {
    if (symbol >= 2)
        throw std::out_of_range("BinaryFrequencyTable symbol");
    return symbol == 0 ? freq0_ : freq1_;
}

void BinaryFrequencyTable::set(std::uint32_t, std::uint32_t) {
    throw std::logic_error("BinaryFrequencyTable is immutable");
}

void BinaryFrequencyTable::increment(std::uint32_t) {
    throw std::logic_error("BinaryFrequencyTable is immutable");
}

std::uint32_t BinaryFrequencyTable::getTotal() const {
    return kTotal;
}

std::uint32_t BinaryFrequencyTable::getLow(std::uint32_t symbol) const {
    if (symbol >= 2)
        throw std::out_of_range("BinaryFrequencyTable symbol");
    return symbol == 0 ? 0u : freq0_;
}

std::uint32_t BinaryFrequencyTable::getHigh(std::uint32_t symbol) const {
    if (symbol >= 2)
        throw std::out_of_range("BinaryFrequencyTable symbol");
    return symbol == 0 ? freq0_ : kTotal;
}

std::uint32_t BinaryFrequencyTable::findSymbol(std::uint32_t value) const {
    if (value >= kTotal)
        throw std::out_of_range("BinaryFrequencyTable value");
    return value < freq0_ ? 0u : 1u;
}


ContextMixModel::ContextMap::ContextMap(std::size_t size_power_of_two)
    : table_(size_power_of_two),
      mask_(size_power_of_two - 1) {}

ContextMixModel::Entry *ContextMixModel::ContextMap::lookup(std::uint32_t key) {
    Entry &entry = table_[ContextMixModel::hash(key) & mask_];
    if (entry.key != key) {
        entry.key = key;
        entry.count0 = 1;
        entry.count1 = 1;
    }
    return &entry;
}


ContextMixModel::ContextMixModel()
    : bit0Counts_{},
      bit1Counts_{},
      sse0Counts_{},
      sse1Counts_{},
      sse2_0Counts_{},
      sse2_1Counts_{},
      ctx0_(1u << 12),
      ctx1_(1u << 16),
      ctx2_(1u << 18),
      ctx3_(1u << 20),
      ctx4_(1u << 22),
      runCtx_(1u << 17),
      activeEntries_{},
      lastSseBucket_(0),
      lastSse2Bucket_(0),
      lastMixedProb_(BinaryFrequencyTable::kTotal / 2),
      weights_{1 * kWeightScale, 2 * kWeightScale, 5 * kWeightScale, 8 * kWeightScale, 10 * kWeightScale, 8 * kWeightScale, 6 * kWeightScale},
      lastProbs_{},
      prev0_(0),
      prev1_(0),
      prev2_(0),
      prev3_(0),
      currentByte_(0),
      bitPos_(0),
      runLength_(0) {
    bit0Counts_.fill(1);
    bit1Counts_.fill(1);
    for (auto &row : sse0Counts_)
        row.fill(1);
    for (auto &row : sse1Counts_)
        row.fill(1);
    for (auto &nibble : sse2_0Counts_)
        for (auto &row : nibble)
            row.fill(1);
    for (auto &nibble : sse2_1Counts_)
        for (auto &row : nibble)
            row.fill(1);
}

// Logit (log-odds) of a probability in [1, kTotal-1] → float
static float stretch(std::uint32_t p) {
    constexpr float kT = static_cast<float>(BinaryFrequencyTable::kTotal);
    float fp = static_cast<float>(p) / kT;
    fp = std::max(fp, 1e-6f);
    fp = std::min(fp, 1.0f - 1e-6f);
    return std::log(fp / (1.0f - fp));
}

// Inverse logit → probability in [1, kTotal-1]
static std::uint32_t squash(float x) {
    constexpr float kT = static_cast<float>(BinaryFrequencyTable::kTotal);
    float p = 1.0f / (1.0f + std::exp(-x));
    std::uint32_t result = static_cast<std::uint32_t>(p * kT);
    if (result == 0) result = 1;
    if (result >= BinaryFrequencyTable::kTotal) result = BinaryFrequencyTable::kTotal - 1;
    return result;
}

std::uint32_t ContextMixModel::hash(std::uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

std::uint32_t ContextMixModel::estimate(const Entry &entry) {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(entry.count1) * BinaryFrequencyTable::kTotal) /
        (entry.count0 + entry.count1));
}

void ContextMixModel::updateEntry(Entry &entry, std::uint32_t bit) {
    if (bit == 0)
        entry.count0++;
    else
        entry.count1++;

    if (entry.count0 + entry.count1 > 1024) {
        entry.count0 = static_cast<std::uint16_t>((entry.count0 + 1) >> 1);
        entry.count1 = static_cast<std::uint16_t>((entry.count1 + 1) >> 1);
    }
}

std::uint32_t ContextMixModel::prefixKey() const {
    return (1u << bitPos_) | currentByte_;
}

std::uint32_t ContextMixModel::predict() const {
    std::uint32_t prefix = prefixKey();
    std::uint32_t baseCtx = (static_cast<std::uint32_t>(bitPos_) << 9) | prefix;
    std::uint32_t runBucket = std::min<std::uint32_t>(runLength_, 63u);

    activeEntries_[0] = const_cast<ContextMap &>(ctx0_).lookup(baseCtx);
    activeEntries_[1] = const_cast<ContextMap &>(ctx1_).lookup(
        (static_cast<std::uint32_t>(prev0_) << 12) ^ baseCtx ^ 0x13579bdu);
    activeEntries_[2] = const_cast<ContextMap &>(ctx2_).lookup(
        (static_cast<std::uint32_t>(prev1_) << 20) ^
        (static_cast<std::uint32_t>(prev0_) << 8) ^ baseCtx ^ 0x2468aceu);
    activeEntries_[3] = const_cast<ContextMap &>(ctx3_).lookup(
        (static_cast<std::uint32_t>(prev2_) << 24) ^
        (static_cast<std::uint32_t>(prev1_) << 16) ^
        (static_cast<std::uint32_t>(prev0_) << 8) ^ baseCtx ^ 0x9e3779b9u);
    activeEntries_[4] = const_cast<ContextMap &>(ctx4_).lookup(
        (static_cast<std::uint32_t>(prev3_) << 24) ^
        (static_cast<std::uint32_t>(prev2_) << 16) ^
        (static_cast<std::uint32_t>(prev1_) << 8) ^
        (static_cast<std::uint32_t>(prev0_)) ^ baseCtx ^ 0xb7e15163u);
    activeEntries_[5] = const_cast<ContextMap &>(runCtx_).lookup(
        (runBucket << 16) ^ (static_cast<std::uint32_t>(prev0_) << 8) ^ baseCtx ^ 0xa511e9b3u);

    const std::uint32_t stationaryIndex = bitPos_ * 2u + ((prev0_ >> 7) & 1u);
    const std::uint32_t stationaryProb =
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(bit1Counts_[stationaryIndex]) * BinaryFrequencyTable::kTotal) /
            (bit0Counts_[stationaryIndex] + bit1Counts_[stationaryIndex]));

    // Gather per-model probabilities (7 models: stationary + ctx0..ctx4 + runCtx)
    lastProbs_[0] = stationaryProb;
    lastProbs_[1] = estimate(*activeEntries_[0]);
    lastProbs_[2] = estimate(*activeEntries_[1]);
    lastProbs_[3] = estimate(*activeEntries_[2]);
    lastProbs_[4] = estimate(*activeEntries_[3]);
    lastProbs_[5] = estimate(*activeEntries_[4]);
    lastProbs_[6] = estimate(*activeEntries_[5]);

    // Mix in logit domain using adaptive weights
    constexpr float kWS = static_cast<float>(kWeightScale);
    float weightSum = 0.0f;
    float logitMix = 0.0f;
    for (int i = 0; i < 7; ++i) {
        float w = static_cast<float>(weights_[i]) / kWS;
        logitMix += w * stretch(lastProbs_[i]);
        weightSum += w;
    }
    if (weightSum > 0.0f)
        logitMix /= weightSum;

    std::uint32_t mixedProb = squash(logitMix);

    const std::uint32_t bucket =
        std::min<std::uint32_t>((mixedProb * kSseBuckets) / BinaryFrequencyTable::kTotal,
                                static_cast<std::uint32_t>(kSseBuckets - 1));
    const std::uint32_t sseProb =
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(sse1Counts_[bitPos_][bucket]) * BinaryFrequencyTable::kTotal) /
            (sse0Counts_[bitPos_][bucket] + sse1Counts_[bitPos_][bucket]));

    lastSseBucket_ = static_cast<std::uint8_t>(bucket);
    lastMixedProb_ = mixedProb;

    // Second SSE layer conditioned on prev0 high nibble
    const std::uint32_t nibble = (prev0_ >> 4) & 0xFu;
    const std::uint32_t sse2Prob =
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(sse2_1Counts_[nibble][bitPos_][bucket]) * BinaryFrequencyTable::kTotal) /
            (sse2_0Counts_[nibble][bitPos_][bucket] + sse2_1Counts_[nibble][bitPos_][bucket]));
    lastSse2Bucket_ = static_cast<std::uint8_t>(bucket);

    // Blend: sse1 from first layer, sse2 from second layer
    std::uint32_t prob = (2u * sseProb + sse2Prob) / 3u;
    if (prob == 0)
        prob = 1;
    if (prob >= BinaryFrequencyTable::kTotal)
        prob = BinaryFrequencyTable::kTotal - 1;
    return prob;
}

void ContextMixModel::update(std::uint32_t bit) {
    const std::uint32_t stationaryIndex = bitPos_ * 2u + ((prev0_ >> 7) & 1u);
    if (bit == 0)
        bit0Counts_[stationaryIndex]++;
    else
        bit1Counts_[stationaryIndex]++;

    if (bit0Counts_[stationaryIndex] + bit1Counts_[stationaryIndex] > 2048) {
        bit0Counts_[stationaryIndex] = static_cast<std::uint16_t>((bit0Counts_[stationaryIndex] + 1) >> 1);
        bit1Counts_[stationaryIndex] = static_cast<std::uint16_t>((bit1Counts_[stationaryIndex] + 1) >> 1);
    }

    if (bit == 0)
        sse0Counts_[bitPos_][lastSseBucket_]++;
    else
        sse1Counts_[bitPos_][lastSseBucket_]++;

    if (sse0Counts_[bitPos_][lastSseBucket_] + sse1Counts_[bitPos_][lastSseBucket_] > 1024) {
        sse0Counts_[bitPos_][lastSseBucket_] =
            static_cast<std::uint16_t>((sse0Counts_[bitPos_][lastSseBucket_] + 1) >> 1);
        sse1Counts_[bitPos_][lastSseBucket_] =
            static_cast<std::uint16_t>((sse1Counts_[bitPos_][lastSseBucket_] + 1) >> 1);
    }

    // Update second SSE layer
    const std::uint32_t nibble = (prev0_ >> 4) & 0xFu;
    if (bit == 0)
        sse2_0Counts_[nibble][bitPos_][lastSse2Bucket_]++;
    else
        sse2_1Counts_[nibble][bitPos_][lastSse2Bucket_]++;

    if (sse2_0Counts_[nibble][bitPos_][lastSse2Bucket_] + sse2_1Counts_[nibble][bitPos_][lastSse2Bucket_] > 1024) {
        sse2_0Counts_[nibble][bitPos_][lastSse2Bucket_] =
            static_cast<std::uint16_t>((sse2_0Counts_[nibble][bitPos_][lastSse2Bucket_] + 1) >> 1);
        sse2_1Counts_[nibble][bitPos_][lastSse2Bucket_] =
            static_cast<std::uint16_t>((sse2_1Counts_[nibble][bitPos_][lastSse2Bucket_] + 1) >> 1);
    }

    for (Entry *entry : activeEntries_)
        updateEntry(*entry, bit);

    // Update adaptive weights: reward models that predicted the actual bit well
    // Gradient step: w_i += lr * stretch(p_i) * (bit - p_i/kTotal)
    // Simplified: w_i += lr * (bit==1 ? p_i : kTotal-p_i) - increase weight if model was right
    constexpr std::int32_t kLR = kWeightScale / 128;  // learning rate ~0.0078
    for (int i = 0; i < 7; ++i) {
        // error signal: +1 if model was correct (pred high and bit=1, or pred low and bit=0), -1 otherwise
        std::int32_t pred = static_cast<std::int32_t>(lastProbs_[i]);
        std::int32_t kT = static_cast<std::int32_t>(BinaryFrequencyTable::kTotal);
        std::int32_t error = (bit == 1) ? pred : (kT - pred);  // higher = model was more right
        // Normalize error to [-kT/2, kT/2] → apply as delta
        std::int32_t delta = kLR * (error - kT / 2) / (kT / 2);
        weights_[i] += delta;
        // Clamp weights to [kWeightScale/16, kWeightScale*16]
        if (weights_[i] < kWeightScale / 16) weights_[i] = kWeightScale / 16;
        if (weights_[i] > kWeightScale * 16) weights_[i] = kWeightScale * 16;
    }

    currentByte_ = static_cast<std::uint8_t>((currentByte_ << 1) | (bit & 1u));
    bitPos_++;
    if (bitPos_ == 8) {
        if (currentByte_ == prev0_) {
            if (runLength_ < 65535)
                runLength_++;
        } else {
            runLength_ = 0;
        }
        prev3_ = prev2_;
        prev2_ = prev1_;
        prev1_ = prev0_;
        prev0_ = currentByte_;
        currentByte_ = 0;
        bitPos_ = 0;
    }
}
