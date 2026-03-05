/*
 * Reference arithmetic coding
 *
 * Copyright (c) Project Nayuki
 * MIT License. See readme file.
 * https://www.nayuki.io/page/reference-arithmetic-coding
 */

#include <limits>
#include <stdexcept>
#include <string>
#include "BitIoStream.hpp"


BitInputStream::BitInputStream(std::istream &in) :
	input(in),
	currentByte(0),
	numBitsRemaining(0) {}


int BitInputStream::read() {
	if (currentByte == std::char_traits<char>::eof())
		return -1;
	if (numBitsRemaining == 0) {
		currentByte = input.get();
		if (currentByte == std::char_traits<char>::eof())
			return -1;
#ifndef NDEBUG
		if (!(0 <= currentByte && currentByte <= 255))
			throw std::logic_error("Assertion error");
#endif
		numBitsRemaining = 8;
	}
#ifndef NDEBUG
	if (numBitsRemaining <= 0)
		throw std::logic_error("Assertion error");
#endif
	numBitsRemaining--;
	return (currentByte >> numBitsRemaining) & 1;
}


int BitInputStream::readNoEof() {
	int result = read();
	if (result != -1)
		return result;
	else
		throw std::runtime_error("End of stream");
}


BitOutputStream::BitOutputStream(std::ostream &out) :
	output(out),
	currentByte(0),
	numBitsFilled(0) {}



void BitOutputStream::finish() {
	while (numBitsFilled != 0)
		write(0);
}
