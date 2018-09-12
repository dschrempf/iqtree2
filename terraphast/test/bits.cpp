
#include <catch.hpp>

#include "../lib/bits.hpp"

namespace terraces {
namespace tests {

using bits::popcount;
using bits::bitscan;
using bits::rbitscan;

TEST_CASE("popcount", "[bits]") {
	CHECK(popcount(0b1010111010101101010001010100000101000011000010111100000101001000) == 26);
	CHECK(popcount(0b0000000000000000000000000000000000000000000000000000000000000000) == 0);
	CHECK(popcount(0b0000001000000100000000000010000000001000000000001000000010001100) == 8);
}

TEST_CASE("bitscan", "[bits]") {
	CHECK(bitscan(0b0100000000000000000000000000000000000000000000000000000000000000) == 62);
	CHECK(bitscan(0b0000000000000000000000000000000000000000000000000000100000000000) == 11);
	CHECK(bitscan(0b0001000000000000010000000000000000000000000000000000000000000000) == 46);

	CHECK(rbitscan(0b0100000000000000000000000000000000000000000000000000000000000000) == 62);
	CHECK(rbitscan(0b0000000000000000000000000000000000000000000000000000100000000000) == 11);
	CHECK(rbitscan(0b0001000000000000010000000000000000000000000000000000000000000000) == 60);
}

} // namespace tests
} // namespace terraces