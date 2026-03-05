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

#include <cstddef>
#include <stdexcept>
#include "PpmModel.hpp"

using std::uint32_t;
using std::vector;


// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

PpmModel::Context::Context(uint32_t symbols, bool hasSubctx_)
    : frequencies(symbols),
      hasSubctx(hasSubctx_)
{
    if (hasSubctx) {
        subcontexts.resize(symbols);   // all slots initialised to nullptr
    }
}


// ---------------------------------------------------------------------------
// PpmModel
// ---------------------------------------------------------------------------

PpmModel::PpmModel(int order, uint32_t symLimit, uint32_t escapeSym, std::size_t maxNodes)
    : modelOrder(order),
      symbolLimit(symLimit),
      escapeSymbol(escapeSym),
      nodeCount(1),     // rootContext counts as the first node
      nodeLimit(maxNodes),
      rootContext(nullptr),
      orderMinus1Freqs(FlatFrequencyTable(symbolLimit))
{
    if (!(order >= -1 && escapeSym < symLimit))
        throw std::domain_error("Illegal argument");
    if (order >= 0) {
        rootContext.reset(new Context(symbolLimit, order >= 1));
        rootContext->frequencies.increment(escapeSymbol);
    }
}


void PpmModel::incrementContexts(const std::deque<uint32_t> &history, uint32_t symbol) {
    if (modelOrder == -1)
        return;
    if (!(history.size() <= static_cast<std::size_t>(modelOrder) && symbol < symbolLimit))
        throw std::invalid_argument("Illegal argument");

    Context *ctx = rootContext.get();
    // PPMC: bump escape count on first occurrence of symbol in this context.
    if (ctx->frequencies.get(symbol) == 0)
        ctx->frequencies.increment(escapeSymbol);
    ctx->frequencies.increment(symbol);

    std::size_t i = 0;
    for (uint32_t sym : history) {
        if (!ctx->hasSubctx)
            throw std::logic_error("Assertion error");

        std::unique_ptr<Context> &subctx = ctx->subcontexts.at(sym);
        if (subctx == nullptr) {
            if (nodeCount >= nodeLimit)
                break;  // node cap reached — stop expanding the trie
            bool childHasSubctx = (i + 1 < static_cast<std::size_t>(modelOrder));
            subctx.reset(new Context(symbolLimit, childHasSubctx));
            subctx->frequencies.increment(escapeSymbol);
            ++nodeCount;
        }
        ctx = subctx.get();
        // PPMC: bump escape count on first occurrence of symbol in this context.
        if (ctx->frequencies.get(symbol) == 0)
            ctx->frequencies.increment(escapeSymbol);
        ctx->frequencies.increment(symbol);
        i++;
    }
}
