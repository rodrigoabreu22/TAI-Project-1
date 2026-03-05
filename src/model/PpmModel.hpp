/*
 * Reference arithmetic coding — PPM model
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 *
 * Ported to this project from the Nayuki C++ reference implementation.
 * Original source: https://github.com/nayuki/Reference-arithmetic-coding
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>
#include "coder/FrequencyTable.hpp"
#include "coder/FenwickFrequencyTable.hpp"


/*
 * Prediction by Partial Matching (PPM) context model.
 *
 * The model maintains a trie of contexts up to depth `modelOrder`.
 * Each trie node holds a FenwickFrequencyTable over `symbolLimit` symbols,
 * giving O(log N) increment/getLow/getHigh instead of O(N) with SimpleFrequencyTable.
 * Symbol `escapeSymbol` serves as both the context-escape and (at order -1) EOF.
 *
 * With adaptive alphabet, symbolLimit = k+1 where k = distinct bytes in the input
 * (k <= 256).  This shrinks the subcontexts vector from 257 to k+1 slots, reducing
 * per-node memory by up to 64% on ASCII text and allowing larger node limits.
 */
class PpmModel final {

    /*---- Inner type ----*/

public:
    class Context final {
    public:
        FenwickFrequencyTable frequencies;
        bool hasSubctx;   // true for non-leaf nodes (depth < modelOrder)
        std::vector<std::unique_ptr<Context>> subcontexts;

        explicit Context(std::uint32_t symbols, bool hasSubctx);
    };


    /*---- Fields ----*/

public:
    int           modelOrder;
    std::uint32_t symbolLimit;    // total alphabet size = k+1 (k data + 1 escape)
    std::uint32_t escapeSymbol;   // compact index of the escape/EOF symbol (= k)

private:
    std::size_t   nodeCount;   // number of Context nodes allocated so far
    std::size_t   nodeLimit;   // hard cap — no new nodes once reached

public:
    std::unique_ptr<Context> rootContext;
    SimpleFrequencyTable orderMinus1Freqs;


    /*---- Constructor ----*/

public:
    // maxNodes: hard cap on Context nodes (rootContext counts as 1).
    // Both compress and decompress must use the same value.
    explicit PpmModel(int order, std::uint32_t symLimit, std::uint32_t escapeSym,
                      std::size_t maxNodes = 1'000'000);


    /*---- Methods ----*/

public:
    // Update all contexts in the history suffix with the given symbol.
    void incrementContexts(const std::deque<std::uint32_t> &history, std::uint32_t symbol);

};
