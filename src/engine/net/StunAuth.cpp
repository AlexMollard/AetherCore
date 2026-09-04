#include "net/StunAuth.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace aether::net::stun::crypto
{
	namespace
	{
		std::uint32_t RotateLeft(std::uint32_t x, unsigned c)
		{
			return (x << c) | (x >> (32u - c));
		}

		// MD5 and SHA-1 padding is the same shape - a single 0x80 byte, zeros out to the
		// last 8 bytes of a 64-byte block, then the bit length - and differ only in which
		// endianness that trailing length is written in (MD5 little, SHA-1 big). Getting
		// that swapped is the classic bug that only shows up for inputs whose length does
		// not happen to be palindromic in its low byte, so the endianness is a parameter
		// here rather than something each caller re-derives.
		std::vector<std::uint8_t> PadMessage(std::span<const std::byte> data, bool bigEndianLength)
		{
			std::vector<std::uint8_t> padded(data.size());
			if (!data.empty())
			{
				std::memcpy(padded.data(), data.data(), data.size());
			}
			padded.push_back(0x80);
			while (padded.size() % 64 != 56)
			{
				padded.push_back(0x00);
			}

			const std::uint64_t bitLength = static_cast<std::uint64_t>(data.size()) * 8u;
			if (bigEndianLength)
			{
				for (int i = 7; i >= 0; --i)
				{
					padded.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));
				}
			}
			else
			{
				for (int i = 0; i < 8; ++i)
				{
					padded.push_back(static_cast<std::uint8_t>(bitLength >> (i * 8)));
				}
			}
			return padded;
		}

		// RFC 1321 s3.4: per-round additive constants, the binary integer part of
		// abs(sin(i + 1)) for i in [0, 64).
		constexpr std::array<std::uint32_t, 64> kMd5K = {
			0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
			0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
			0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
			0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
			0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
			0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
			0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
			0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
		};

		// RFC 1321 s3.4: per-round left-rotate amounts.
		constexpr std::array<unsigned, 64> kMd5S = {
			7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
			5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
			4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
			6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
		};

		constexpr std::array<std::uint32_t, 256> BuildCrc32Table()
		{
			std::array<std::uint32_t, 256> table{};
			for (std::uint32_t i = 0; i < 256; ++i)
			{
				std::uint32_t c = i;
				for (int bit = 0; bit < 8; ++bit)
				{
					c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				}
				table[i] = c;
			}
			return table;
		}

		constexpr std::array<std::uint32_t, 256> kCrc32Table = BuildCrc32Table();
	} // namespace

	Md5Digest Md5(std::span<const std::byte> data)
	{
		std::uint32_t a0 = 0x67452301u;
		std::uint32_t b0 = 0xefcdab89u;
		std::uint32_t c0 = 0x98badcfeu;
		std::uint32_t d0 = 0x10325476u;

		const auto padded = PadMessage(data, /*bigEndianLength=*/false);

		for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64)
		{
			std::array<std::uint32_t, 16> m{};
			for (int i = 0; i < 16; ++i)
			{
				const std::size_t o = chunk + static_cast<std::size_t>(i) * 4;
				m[i] = static_cast<std::uint32_t>(padded[o]) | (static_cast<std::uint32_t>(padded[o + 1]) << 8) | (static_cast<std::uint32_t>(padded[o + 2]) << 16) | (static_cast<std::uint32_t>(padded[o + 3]) << 24);
			}

			std::uint32_t a = a0;
			std::uint32_t b = b0;
			std::uint32_t c = c0;
			std::uint32_t d = d0;

			for (std::uint32_t i = 0; i < 64; ++i)
			{
				std::uint32_t f = 0;
				std::uint32_t g = 0;
				if (i < 16)
				{
					f = (b & c) | (~b & d);
					g = i;
				}
				else if (i < 32)
				{
					f = (d & b) | (~d & c);
					g = (5u * i + 1u) % 16u;
				}
				else if (i < 48)
				{
					f = b ^ c ^ d;
					g = (3u * i + 5u) % 16u;
				}
				else
				{
					f = c ^ (b | ~d);
					g = (7u * i) % 16u;
				}

				f = f + a + kMd5K[i] + m[g];
				a = d;
				d = c;
				c = b;
				b = b + RotateLeft(f, kMd5S[i]);
			}

			a0 += a;
			b0 += b;
			c0 += c;
			d0 += d;
		}

		Md5Digest digest;
		const std::array<std::uint32_t, 4> words = {a0, b0, c0, d0};
		for (std::size_t i = 0; i < words.size(); ++i)
		{
			// MD5 output is little-endian per word (RFC 1321 s3.5).
			digest.bytes[i * 4 + 0] = static_cast<std::uint8_t>(words[i]);
			digest.bytes[i * 4 + 1] = static_cast<std::uint8_t>(words[i] >> 8);
			digest.bytes[i * 4 + 2] = static_cast<std::uint8_t>(words[i] >> 16);
			digest.bytes[i * 4 + 3] = static_cast<std::uint8_t>(words[i] >> 24);
		}
		return digest;
	}

	Sha1Digest Sha1(std::span<const std::byte> data)
	{
		std::uint32_t h0 = 0x67452301u;
		std::uint32_t h1 = 0xEFCDAB89u;
		std::uint32_t h2 = 0x98BADCFEu;
		std::uint32_t h3 = 0x10325476u;
		std::uint32_t h4 = 0xC3D2E1F0u;

		const auto padded = PadMessage(data, /*bigEndianLength=*/true);

		for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64)
		{
			std::array<std::uint32_t, 80> w{};
			for (int i = 0; i < 16; ++i)
			{
				const std::size_t o = chunk + static_cast<std::size_t>(i) * 4;
				w[i] = (static_cast<std::uint32_t>(padded[o]) << 24) | (static_cast<std::uint32_t>(padded[o + 1]) << 16) | (static_cast<std::uint32_t>(padded[o + 2]) << 8) | static_cast<std::uint32_t>(padded[o + 3]);
			}
			for (int i = 16; i < 80; ++i)
			{
				w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
			}

			std::uint32_t a = h0;
			std::uint32_t b = h1;
			std::uint32_t c = h2;
			std::uint32_t d = h3;
			std::uint32_t e = h4;

			for (int i = 0; i < 80; ++i)
			{
				std::uint32_t f = 0;
				std::uint32_t k = 0;
				if (i < 20)
				{
					f = (b & c) | (~b & d);
					k = 0x5A827999u;
				}
				else if (i < 40)
				{
					f = b ^ c ^ d;
					k = 0x6ED9EBA1u;
				}
				else if (i < 60)
				{
					f = (b & c) | (b & d) | (c & d);
					k = 0x8F1BBCDCu;
				}
				else
				{
					f = b ^ c ^ d;
					k = 0xCA62C1D6u;
				}

				const std::uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
				e = d;
				d = c;
				c = RotateLeft(b, 30);
				b = a;
				a = temp;
			}

			h0 += a;
			h1 += b;
			h2 += c;
			h3 += d;
			h4 += e;
		}

		Sha1Digest digest;
		const std::array<std::uint32_t, 5> words = {h0, h1, h2, h3, h4};
		for (std::size_t i = 0; i < words.size(); ++i)
		{
			// SHA-1 output is big-endian per word (RFC 3174 s6.1 step d).
			digest.bytes[i * 4 + 0] = static_cast<std::uint8_t>(words[i] >> 24);
			digest.bytes[i * 4 + 1] = static_cast<std::uint8_t>(words[i] >> 16);
			digest.bytes[i * 4 + 2] = static_cast<std::uint8_t>(words[i] >> 8);
			digest.bytes[i * 4 + 3] = static_cast<std::uint8_t>(words[i]);
		}
		return digest;
	}

	Sha1Digest HmacSha1(std::span<const std::byte> key, std::span<const std::byte> data)
	{
		constexpr std::size_t kBlockSize = 64;

		// RFC 2104 s2: a key longer than the block size is hashed down first; a key
		// shorter than the block size is zero-padded up to it. std::array's value
		// initialization already zero-fills the remainder for the short-key path, so
		// there is no separate padding step to get wrong here.
		std::array<std::byte, kBlockSize> keyBlock{};
		if (key.size() > kBlockSize)
		{
			const Sha1Digest hashed = Sha1(key);
			std::memcpy(keyBlock.data(), hashed.bytes.data(), hashed.bytes.size());
		}
		else if (!key.empty())
		{
			std::memcpy(keyBlock.data(), key.data(), key.size());
		}

		std::array<std::byte, kBlockSize> ipad{};
		std::array<std::byte, kBlockSize> opad{};
		for (std::size_t i = 0; i < kBlockSize; ++i)
		{
			const auto k = static_cast<std::uint8_t>(keyBlock[i]);
			ipad[i] = static_cast<std::byte>(k ^ 0x36u);
			opad[i] = static_cast<std::byte>(k ^ 0x5cu);
		}

		std::vector<std::byte> inner(ipad.begin(), ipad.end());
		inner.insert(inner.end(), data.begin(), data.end());
		const Sha1Digest innerHash = Sha1(inner);

		std::vector<std::byte> outer(opad.begin(), opad.end());
		const auto innerHashBytes = std::as_bytes(std::span(innerHash.bytes));
		outer.insert(outer.end(), innerHashBytes.begin(), innerHashBytes.end());

		return Sha1(outer);
	}

	std::uint32_t Crc32(std::span<const std::byte> data)
	{
		std::uint32_t crc = 0xFFFFFFFFu;
		for (const std::byte b: data)
		{
			const std::uint8_t index = static_cast<std::uint8_t>(crc ^ static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)));
			crc = kCrc32Table[index] ^ (crc >> 8);
		}
		return crc ^ 0xFFFFFFFFu;
	}

	Md5Digest LongTermKey(std::string_view username, std::string_view realm, std::string_view password)
	{
		std::string joined;
		joined.reserve(username.size() + realm.size() + password.size() + 2);
		joined.append(username);
		joined.push_back(':');
		joined.append(realm);
		joined.push_back(':');
		joined.append(password);
		return Md5(std::as_bytes(std::span<const char>(joined)));
	}
} // namespace aether::net::stun::crypto
