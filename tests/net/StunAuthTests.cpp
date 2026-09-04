#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "net/StunAuth.hpp"

using namespace aether::net::stun::crypto;

namespace
{
	std::span<const std::byte> AsBytes(const std::string& s)
	{
		return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
	}

	std::span<const std::byte> AsBytes(const std::vector<std::uint8_t>& v)
	{
		return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
	}

	// Decodes a hex string (as RFC 2202's test vectors give both keys and digests) into
	// raw bytes, so a vector can be transcribed the way the RFC prints it.
	std::vector<std::uint8_t> FromHex(std::string_view hex)
	{
		std::vector<std::uint8_t> out;
		out.reserve(hex.size() / 2);
		for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
		{
			out.push_back(static_cast<std::uint8_t>(std::stoul(std::string(hex.substr(i, 2)), nullptr, 16)));
		}
		return out;
	}

	std::string ToHex(std::span<const std::uint8_t> bytes)
	{
		static constexpr char kDigits[] = "0123456789abcdef";
		std::string out;
		out.reserve(bytes.size() * 2);
		for (const std::uint8_t b: bytes)
		{
			out.push_back(kDigits[b >> 4]);
			out.push_back(kDigits[b & 0x0F]);
		}
		return out;
	}

	std::string ToHex(const Md5Digest& d)
	{
		return ToHex(std::span(d.bytes));
	}

	std::string ToHex(const Sha1Digest& d)
	{
		return ToHex(std::span(d.bytes));
	}
} // namespace

// --- MD5, RFC 1321 Appendix A.5 test suite -------------------------------------------

TEST_CASE("MD5 matches the RFC 1321 test suite")
{
	CHECK(ToHex(Md5(AsBytes(std::string("")))) == "d41d8cd98f00b204e9800998ecf8427e");
	CHECK(ToHex(Md5(AsBytes(std::string("a")))) == "0cc175b9c0f1b6a831c399e269772661");
	CHECK(ToHex(Md5(AsBytes(std::string("abc")))) == "900150983cd24fb0d6963f7d28e17f72");
	CHECK(ToHex(Md5(AsBytes(std::string("message digest")))) == "f96b697d7cb7938d525a2f31aaf161d0");
	CHECK(ToHex(Md5(AsBytes(std::string("abcdefghijklmnopqrstuvwxyz")))) == "c3fcd3d76192e4007dfb496cca67e13b");
	CHECK(ToHex(Md5(AsBytes(std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")))) == "d174ab98d277d9f5a5611c2c9f419d9f");
	CHECK(ToHex(Md5(AsBytes(std::string("12345678901234567890123456789012345678901234567890123456789012345678901234567890")))) == "57edf4a22be3c955ac49da2e2107b67a");
}

// --- SHA-1, RFC 3174 s7.3 test vectors ------------------------------------------------

TEST_CASE("SHA-1 matches the RFC 3174 test vectors")
{
	CHECK(ToHex(Sha1(AsBytes(std::string("")))) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK(ToHex(Sha1(AsBytes(std::string("abc")))) == "a9993e364706816aba3e25717850c26c9cd0d89d");
	CHECK(ToHex(Sha1(AsBytes(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

// --- HMAC-SHA1, RFC 2202 test cases 1-7 -----------------------------------------------
// Cases 1-5 use keys at or under the 64-byte block size (the zero-pad path); cases 6-7
// use an 80-byte key (the hash-the-key-first path) - both branches of RFC 2104 s2.

TEST_CASE("HMAC-SHA1 matches the RFC 2202 test vectors")
{
	// Case 1: key = 20 bytes of 0x0b, data = "Hi There".
	{
		const std::vector<std::uint8_t> key(20, 0x0b);
		const auto mac = HmacSha1(AsBytes(key), AsBytes(std::string("Hi There")));
		CHECK(ToHex(mac) == "b617318655057264e28bc0b6fb378c8ef146be00");
	}
	// Case 2: key = "Jefe", data = "what do ya want for nothing?".
	{
		const auto mac = HmacSha1(AsBytes(std::string("Jefe")), AsBytes(std::string("what do ya want for nothing?")));
		CHECK(ToHex(mac) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
	}
	// Case 3: key = 20 bytes of 0xaa, data = 50 bytes of 0xdd.
	{
		const std::vector<std::uint8_t> key(20, 0xaa);
		const std::vector<std::uint8_t> data(50, 0xdd);
		const auto mac = HmacSha1(AsBytes(key), AsBytes(data));
		CHECK(ToHex(mac) == "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
	}
	// Case 4: key = 25 bytes 0x01..0x19, data = 50 bytes of 0xcd.
	{
		const auto key = FromHex("0102030405060708090a0b0c0d0e0f10111213141516171819");
		const std::vector<std::uint8_t> data(50, 0xcd);
		const auto mac = HmacSha1(AsBytes(key), AsBytes(data));
		CHECK(ToHex(mac) == "4c9007f4026250c6bc8414f9bf50c86c2d7235da");
	}
	// Case 5: key = 20 bytes of 0x0c, data = "Test With Truncation" (unwrapped, 20-byte
	// output - RFC 2202 also truncates this one to 96 bits, which is a caller concern
	// downstream of this function, not something HmacSha1 itself does).
	{
		const std::vector<std::uint8_t> key(20, 0x0c);
		const auto mac = HmacSha1(AsBytes(key), AsBytes(std::string("Test With Truncation")));
		CHECK(ToHex(mac) == "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04");
	}
	// Case 6: key = 80 bytes of 0xaa (longer than the 64-byte block - exercises the
	// hash-the-key-first branch), data = "Test Using Larger Than Block-Size Key - Hash Key First".
	{
		const std::vector<std::uint8_t> key(80, 0xaa);
		const auto mac = HmacSha1(AsBytes(key), AsBytes(std::string("Test Using Larger Than Block-Size Key - Hash Key First")));
		CHECK(ToHex(mac) == "aa4ae5e15272d00e95705637ce8a3b55ed402112");
	}
	// Case 7: same over-length key, data longer than one block too.
	{
		const std::vector<std::uint8_t> key(80, 0xaa);
		const auto mac = HmacSha1(AsBytes(key), AsBytes(std::string("Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data")));
		CHECK(ToHex(mac) == "e8e99d0f45237d786d6bbaa7965c7808bbff1a91");
	}
}

// --- CRC-32, the standard IEEE 802.3 check value --------------------------------------

TEST_CASE("CRC-32 matches the standard check value for \"123456789\"")
{
	// This is the canonical CRC-32/ISO-HDLC check value quoted by every implementation
	// reference (RFC 5389's FINGERPRINT uses this same reflected polynomial).
	CHECK(Crc32(AsBytes(std::string("123456789"))) == 0xCBF43926u);
}

// --- Block-boundary lengths: MD5 and SHA-1 pad at the 56-byte mark within a 64-byte
// block, so 55/56/57 bytes exercise "just fits", "padding spills into a second block",
// and "one byte past that" - and 63/64/65 exercise the same for a full extra block.
// Independently-known-correct values below, cross-checked against a second, unrelated
// implementation (Python's hashlib) rather than against this code's own output.

TEST_CASE("MD5 and SHA-1 handle inputs at and around block boundaries")
{
	const std::string s55(55, 'A');
	const std::string s56(56, 'A');
	const std::string s57(57, 'A');
	const std::string s63(63, 'A');
	const std::string s64(64, 'A');
	const std::string s65(65, 'A');
	const std::string s128(128, 'A');

	CHECK(ToHex(Md5(AsBytes(s55))) == "e38a93ffe074a99b3fed47dfbe37db21");
	CHECK(ToHex(Md5(AsBytes(s56))) == "a2f3e2024931bd470555002aa5ccc010");
	CHECK(ToHex(Md5(AsBytes(s57))) == "9a7c38569e5a96e3cfbad45fb9ce5209");
	CHECK(ToHex(Md5(AsBytes(s63))) == "5f1c4bb2970471a5c75b7ba1dc9ee3ed");
	CHECK(ToHex(Md5(AsBytes(s64))) == "d289a97565bc2d27ac8b8545a5ddba45");
	CHECK(ToHex(Md5(AsBytes(s65))) == "162b6d6eb17cd9da55f95f8c73a32dda");
	CHECK(ToHex(Md5(AsBytes(s128))) == "af35b0d348e5162036e183339d385b0c");

	CHECK(ToHex(Sha1(AsBytes(s55))) == "5021b3d42aa093bffc34eedd7a1455f3624bc552");
	CHECK(ToHex(Sha1(AsBytes(s56))) == "6b45e3cf1eb3324b9fd4df3b83d89c4c2c4ca896");
	CHECK(ToHex(Sha1(AsBytes(s57))) == "e8d6ea5c627fc8676fa662677b028640844dc35c");
	CHECK(ToHex(Sha1(AsBytes(s63))) == "0ec86b3f3ac34ad860fa8da56bcca03a54018049");
	CHECK(ToHex(Sha1(AsBytes(s64))) == "30b86e44e6001403827a62c58b08893e77cf121f");
	CHECK(ToHex(Sha1(AsBytes(s65))) == "826b7e7a7af8a529ae1c7443c23bf185c0ad440c");
	CHECK(ToHex(Sha1(AsBytes(s128))) == "2927490ade868795ecdd8febe05214cbd243ef35");
}

// --- LongTermKey wiring: RFC 5389 s15.4 defines it as MD5(username ":" realm ":" password).
// Checked against a hand-built colon-joined string hashed independently, not against the
// function reusing its own concatenation logic.

TEST_CASE("LongTermKey is MD5 of username, realm, and password joined by colons")
{
	const Md5Digest key = LongTermKey("user", "realm", "pass");
	CHECK(ToHex(key) == "8493fbc53ba582fb4c044c456bdc40eb");

	// Cross-checked against Md5() applied directly to the same joined bytes, so the test
	// does not merely assert that LongTermKey agrees with a second copy of itself: the
	// hex above was computed by an independent tool (Python hashlib) beforehand.
	CHECK(ToHex(Md5(AsBytes(std::string("user:realm:pass")))) == ToHex(key));
}
