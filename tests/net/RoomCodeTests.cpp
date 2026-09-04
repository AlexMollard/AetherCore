#include <doctest/doctest.h>

#include <cctype>
#include <set>
#include <string>

#include "net/RoomCode.hpp"

using namespace aether;

TEST_CASE("A generated code round-trips through normalization unchanged")
{
	for (int i = 0; i < 20; ++i)
	{
		const std::string code = net::NewRoomCode();
		REQUIRE(code.size() == net::kRoomCodeLength);
		for (const char c: code)
		{
			CHECK((std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c))));
		}

		const auto normalized = net::NormalizeRoomCode(code);
		REQUIRE(normalized.has_value());
		CHECK(*normalized == code);
	}
}

TEST_CASE("Room codes generated close together do not collide")
{
	// The whole point of drawing from a real entropy source rather than seeding
	// from the clock: two players hosting in the same second must land on
	// different codes. 32^6 possibilities makes a collision in a sample this small
	// vanishingly unlikely if the source is doing its job.
	std::set<std::string> codes;
	for (int i = 0; i < 500; ++i)
	{
		codes.insert(net::NewRoomCode());
	}
	CHECK(codes.size() == 500);
}

TEST_CASE("Normalization is case-insensitive and maps confusable letters to what a real code uses")
{
	CHECK(net::NormalizeRoomCode("abcdef") == std::optional<std::string>("ABCDEF"));
	CHECK(net::NormalizeRoomCode("a1b2c3") == std::optional<std::string>("A1B2C3"));

	// Whatever a generated code happens to contain, typing I/l for 1 and O for 0
	// must still read back as the code that was actually handed out - that is the
	// entire reason those letters are the ones left out of the alphabet.
	const std::string code = net::NewRoomCode();
	std::string confused = code;
	for (char& c: confused)
	{
		if (c == '1')
		{
			c = 'I';
		}
		else if (c == '0')
		{
			c = 'O';
		}
	}
	const auto normalized = net::NormalizeRoomCode(confused);
	REQUIRE(normalized.has_value());
	CHECK(*normalized == code);
}

TEST_CASE("Spaces and dashes read out for pronounceability are stripped before validation")
{
	CHECK(net::NormalizeRoomCode("AB-CD EF") == std::optional<std::string>("ABCDEF"));
	CHECK(net::NormalizeRoomCode("A B-C D-E F") == std::optional<std::string>("ABCDEF"));
	CHECK(net::NormalizeRoomCode("--AB CDEF--") == std::optional<std::string>("ABCDEF"));
}

TEST_CASE("Anything that is not a plausible room code is refused, not salvaged")
{
	CHECK_FALSE(net::NormalizeRoomCode("").has_value());
	CHECK_FALSE(net::NormalizeRoomCode("ABCDE").has_value());   // one short
	CHECK_FALSE(net::NormalizeRoomCode("ABCDEFG").has_value()); // one long
	CHECK_FALSE(net::NormalizeRoomCode("AB--EF").has_value());  // four real characters once dashes strip
	// U is excluded from the alphabet outright, not a confusable stand-in for
	// anything in it - unlike I/l and O, it must not be silently accepted.
	CHECK_FALSE(net::NormalizeRoomCode("ABCDEU").has_value());
	CHECK_FALSE(net::NormalizeRoomCode("AB!DEF").has_value()); // punctuation beyond space/dash
	CHECK_FALSE(net::NormalizeRoomCode("ABCDE.").has_value());
}
