/*
 * Reference arithmetic coding
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 */

#pragma once

#include <istream>
#include <limits>
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

	// Writes a single bit (0 or 1). Inlined for performance — called ~73M times.
	public: inline void write(int b) {
		currentByte = (currentByte << 1) | b;
		numBitsFilled++;
		if (numBitsFilled == 8) {
			if (std::numeric_limits<char>::is_signed)
				currentByte -= (currentByte >> 7) << 8;
			output.put(static_cast<char>(currentByte));
			currentByte = 0;
			numBitsFilled = 0;
		}
	}

	// Flushes remaining bits with zero padding to reach a byte boundary.
	public: void finish();

};
