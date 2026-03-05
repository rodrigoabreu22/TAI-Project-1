/*
 * Reference arithmetic coding
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 */

#include <limits>
#include <stdexcept>
#include "ArithmeticCoder.hpp"

using std::uint32_t;
using std::uint64_t;


ArithmeticCoderBase::ArithmeticCoderBase(int numBits) {
	if (!(1 <= numBits && numBits <= 63))
		throw std::domain_error("State size out of range");
	numStateBits = numBits;
	fullRange     = static_cast<uint64_t>(1) << numStateBits;
	halfRange     = fullRange >> 1;
	quarterRange  = halfRange >> 1;
	minimumRange  = quarterRange + 2;
	maximumTotal  = std::min(std::numeric_limits<uint64_t>::max() / fullRange, minimumRange);
	stateMask     = fullRange - 1;
	low  = 0;
	high = stateMask;
}

ArithmeticCoderBase::~ArithmeticCoderBase() {}


void ArithmeticCoderBase::update(const FrequencyTable &freqs, uint32_t symbol) {
#ifndef NDEBUG
	if (low >= high || (low & stateMask) != low || (high & stateMask) != high)
		throw std::logic_error("Assertion error: Low or high out of range");
#endif
	uint64_t range = high - low + 1;
#ifndef NDEBUG
	if (!(minimumRange <= range && range <= fullRange))
		throw std::logic_error("Assertion error: Range out of range");
#endif

	uint32_t total  = freqs.getTotal();
	uint32_t symLow = freqs.getLow(symbol);
	uint32_t symHigh = freqs.getHigh(symbol);
	if (symLow == symHigh)
		throw std::invalid_argument("Symbol has zero frequency");
	if (total > maximumTotal)
		throw std::invalid_argument("Cannot code symbol because total is too large");

	uint64_t newLow  = low + symLow  * range / total;
	uint64_t newHigh = low + symHigh * range / total - 1;
	low  = newLow;
	high = newHigh;

	while (((low ^ high) & halfRange) == 0) {
		shift();
		low  = ((low  << 1) & stateMask);
		high = ((high << 1) & stateMask) | 1;
	}
	while ((low & ~high & quarterRange) != 0) {
		underflow();
		low  = (low << 1) ^ halfRange;
		high = ((high ^ halfRange) << 1) | halfRange | 1;
	}
}


ArithmeticDecoder::ArithmeticDecoder(int numBits, BitInputStream &in) :
		ArithmeticCoderBase(numBits),
		input(in),
		code(0) {
	for (int i = 0; i < numStateBits; i++)
		code = code << 1 | readCodeBit();
}


uint32_t ArithmeticDecoder::read(const FrequencyTable &freqs) {
	uint32_t total = freqs.getTotal();
	if (total > maximumTotal)
		throw std::invalid_argument("Cannot decode symbol because total is too large");
	uint64_t range  = high - low + 1;
	uint64_t offset = code - low;
	uint64_t value  = ((offset + 1) * total - 1) / range;
#ifndef NDEBUG
	if (value * range / total > offset)
		throw std::logic_error("Assertion error");
	if (value >= total)
		throw std::logic_error("Assertion error");
#endif

	// O(log N) symbol lookup via virtual findSymbol().
	// FenwickFrequencyTable overrides this with a single Fenwick-tree descent,
	// replacing the previous O(log^2 N) binary-search + getLow loop.
	uint32_t symbol = freqs.findSymbol(static_cast<uint32_t>(value));
#ifndef NDEBUG
	if (!(freqs.getLow(symbol) * range / total <= offset &&
	      offset < freqs.getHigh(symbol) * range / total))
		throw std::logic_error("Assertion error");
#endif
	update(freqs, symbol);
#ifndef NDEBUG
	if (!(low <= code && code <= high))
		throw std::logic_error("Assertion error: Code out of range");
#endif
	return symbol;
}

void ArithmeticDecoder::shift() {
	code = ((code << 1) & stateMask) | readCodeBit();
}

void ArithmeticDecoder::underflow() {
	code = (code & halfRange) | ((code << 1) & (stateMask >> 1)) | readCodeBit();
}

int ArithmeticDecoder::readCodeBit() {
	int temp = input.read();
	if (temp == -1) temp = 0;
	return temp;
}


ArithmeticEncoder::ArithmeticEncoder(int numBits, BitOutputStream &out) :
	ArithmeticCoderBase(numBits),
	output(out),
	numUnderflow(0) {}

void ArithmeticEncoder::write(const FrequencyTable &freqs, uint32_t symbol) {
	update(freqs, symbol);
}

void ArithmeticEncoder::finish() {
	output.write(1);
}

void ArithmeticEncoder::shift() {
	int bit = static_cast<int>(low >> (numStateBits - 1));
	output.write(bit);
	for (; numUnderflow > 0; numUnderflow--)
		output.write(bit ^ 1);
}

void ArithmeticEncoder::underflow() {
	if (numUnderflow == std::numeric_limits<decltype(numUnderflow)>::max())
		throw std::overflow_error("Maximum underflow reached");
	numUnderflow++;
}
