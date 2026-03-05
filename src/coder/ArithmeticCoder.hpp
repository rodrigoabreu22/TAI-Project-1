/*
 * Reference arithmetic coding
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include "BitIoStream.hpp"
#include "FrequencyTable.hpp"


/*
 * Shared state for arithmetic encoder and decoder.
 * numStateBits=32 is recommended: maximises maximumTotal (~2^30).
 */
class ArithmeticCoderBase {

	protected: int numStateBits;
	protected: std::uint64_t fullRange;
	protected: std::uint64_t halfRange;
	protected: std::uint64_t quarterRange;
	protected: std::uint64_t minimumRange;
	protected: std::uint64_t maximumTotal;
	protected: std::uint64_t stateMask;
	protected: std::uint64_t low;
	protected: std::uint64_t high;

	public: explicit ArithmeticCoderBase(int numBits);
	public: virtual ~ArithmeticCoderBase() = 0;

	protected: virtual void update(const FrequencyTable &freqs, std::uint32_t symbol);
	protected: virtual void shift() = 0;
	protected: virtual void underflow() = 0;

};



/*
 * Arithmetic decoder: reads a bit stream and returns symbols.
 */
class ArithmeticDecoder final : private ArithmeticCoderBase {

	private: BitInputStream &input;
	private: std::uint64_t code;

	public: explicit ArithmeticDecoder(int numBits, BitInputStream &in);

	public: std::uint32_t read(const FrequencyTable &freqs);

	protected: void shift() override;
	protected: void underflow() override;
	private: int readCodeBit();

};



/*
 * Arithmetic encoder: encodes symbols to a bit stream.
 */
class ArithmeticEncoder final : private ArithmeticCoderBase {

	private: BitOutputStream &output;
	private: unsigned long numUnderflow;

	public: explicit ArithmeticEncoder(int numBits, BitOutputStream &out);

	public: void write(const FrequencyTable &freqs, std::uint32_t symbol);

	// Must be called after the last symbol to flush buffered state bits.
	public: void finish();

	protected: void shift() override;
	protected: void underflow() override;

};
