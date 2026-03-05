/*
 * Reference arithmetic coding
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 */

#pragma once

#include <istream>
#include <ostream>


/*
 * A stream of bits that can be read. Because they come from an underlying byte stream,
 * the total number of bits is always a multiple of 8. The bits are read in big endian.
 */
class BitInputStream final {

	private: std::istream &input;
	private: int currentByte;
	private: int numBitsRemaining;

	public: explicit BitInputStream(std::istream &in);

	// Returns 0 or 1 if a bit is available, or -1 at end of stream.
	public: int read();

	// Returns 0 or 1 if a bit is available, or throws at end of stream.
	public: int readNoEof();

};



/*
 * A stream where bits can be written to. Padded to a byte boundary at finish().
 */
class BitOutputStream final {

	private: std::ostream &output;
	private: int currentByte;
	private: int numBitsFilled;

	public: explicit BitOutputStream(std::ostream &out);

	// Writes a single bit (must be 0 or 1).
	public: void write(int b);

	// Flushes remaining bits with zero padding to reach a byte boundary.
	public: void finish();

};
