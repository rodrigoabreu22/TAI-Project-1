#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

// Forward BWT via cyclic-rotation suffix array (prefix doubling, O(N log²N)).
// Returns (bwt_output, primary_index).
// primary_index is the row in the sorted rotation matrix where the original
// string appears — the only extra piece of information needed for inversion.
inline std::pair<std::vector<uint8_t>, uint32_t>
bwt_forward(const std::vector<uint8_t> &data)
{
    const size_t N = data.size();
    if (N == 0) return {{}, 0};
    if (N == 1) return {{data[0]}, 0};

    std::vector<uint32_t> SA(N);
    std::iota(SA.begin(), SA.end(), 0u);
    std::vector<int32_t> rank_(N), tmp(N);
    for (size_t i = 0; i < N; i++) rank_[i] = data[i];

    for (size_t gap = 1; gap < N; gap *= 2) {
        std::sort(SA.begin(), SA.end(), [&](uint32_t a, uint32_t b) {
            if (rank_[a] != rank_[b]) return rank_[a] < rank_[b];
            return rank_[(a + gap) % N] < rank_[(b + gap) % N];
        });

        tmp[SA[0]] = 0;
        for (size_t i = 1; i < N; i++) {
            tmp[SA[i]] = tmp[SA[i - 1]];
            if (rank_[SA[i]] != rank_[SA[i - 1]] ||
                rank_[(SA[i] + gap) % N] != rank_[(SA[i - 1] + gap) % N])
                tmp[SA[i]]++;
        }
        rank_ = tmp;
        if (rank_[SA[N - 1]] == static_cast<int32_t>(N) - 1) break;
    }

    std::vector<uint8_t> L(N);
    uint32_t primary_index = 0;
    for (size_t i = 0; i < N; i++) {
        if (SA[i] == 0) {
            L[i] = data[N - 1];
            primary_index = static_cast<uint32_t>(i);
        } else {
            L[i] = data[SA[i] - 1];
        }
    }
    return {std::move(L), primary_index};
}

// Inverse BWT via LF-mapping. O(N).
// Given the BWT output L and the primary_index, reconstructs the original data.
inline std::vector<uint8_t>
bwt_inverse(const std::vector<uint8_t> &L, uint32_t primary_index)
{
    const size_t N = L.size();
    if (N == 0) return {};

    // Count occurrences of each symbol in L
    uint32_t cnt[256] = {};
    for (uint8_t b : L) cnt[b]++;

    // C[s] = number of symbols strictly less than s in L
    uint32_t C[256] = {};
    for (int i = 1; i < 256; i++) C[i] = C[i - 1] + cnt[i - 1];

    // LF[i] = the row in the sorted matrix that L[i] maps to
    std::vector<uint32_t> LF(N);
    uint32_t rank_cnt[256] = {};
    for (size_t i = 0; i < N; i++) {
        uint8_t c = L[i];
        LF[i] = C[c] + rank_cnt[c]++;
    }

    // Follow LF-mapping backwards from primary_index to reconstruct original
    std::vector<uint8_t> original(N);
    uint32_t pos = primary_index;
    for (size_t j = N; j-- > 0;) {
        original[j] = L[pos];
        pos = LF[pos];
    }
    return original;
}
