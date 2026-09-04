#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace aether::net::stun::crypto
{
	// MD5 (RFC 1321) and SHA-1 (RFC 3174) live here for exactly one reason: RFC 5389's
	// long-term credential mechanism and RFC 5766's relay authentication mandate them on
	// the wire, and a TURN client that wants to talk to a real server has no say in which
	// hash the protocol uses. Both are cryptographically broken as general-purpose
	// hashes - practical collisions are known for both - and MUST NOT be reached for
	// anything else: not content hashing, not password storage, not anything
	// security-sensitive. A future need for a hash is a new function that says what it
	// is for, never a "reuse" of these two.

	struct Md5Digest
	{
		std::array<std::uint8_t, 16> bytes{};

		// Plain byte-wise comparison (std::array's default, which may stop at the first
		// difference). That is fine here: this authenticates a TURN relay allocation, not
		// a password check, so there is no timing side channel worth paying for. If this
		// digest is ever compared against attacker-controlled secret material, that call
		// site needs a real constant-time compare - this one is not it.
		friend bool operator==(const Md5Digest&, const Md5Digest&) = default;
	};

	struct Sha1Digest
	{
		std::array<std::uint8_t, 20> bytes{};

		// Same non-constant-time comparison, same reasoning as Md5Digest above.
		friend bool operator==(const Sha1Digest&, const Sha1Digest&) = default;
	};

	[[nodiscard]] Md5Digest Md5(std::span<const std::byte> data);
	[[nodiscard]] Sha1Digest Sha1(std::span<const std::byte> data);

	// RFC 2104 HMAC, instantiated with SHA-1. This is what STUN's MESSAGE-INTEGRITY
	// (RFC 5389 s15.4) is: an HMAC-SHA1 over the message up to that attribute, keyed by
	// the long-term credential key below (or, for short-term credentials, the password
	// directly - that policy choice belongs to the caller, not to this function).
	[[nodiscard]] Sha1Digest HmacSha1(std::span<const std::byte> key, std::span<const std::byte> data);

	// The IEEE 802.3 reflected CRC-32 (polynomial 0xEDB88320, as commonly notated in its
	// bit-reflected form), which is what RFC 5389's FINGERPRINT attribute is defined
	// over. Not a security primitive at all - it exists only to let a receiver cheaply
	// tell "this is STUN" from "this collided with something else on the same port".
	[[nodiscard]] std::uint32_t Crc32(std::span<const std::byte> data);

	// RFC 5389 s15.4 long-term credential key: MD5(username ":" realm ":" password). The
	// three fields are joined with a literal colon each, unescaped - the STUN grammar
	// forbids colons inside USERNAME, REALM, and the password source, so there is no
	// ambiguity for an escape to resolve.
	[[nodiscard]] Md5Digest LongTermKey(std::string_view username, std::string_view realm, std::string_view password);
} // namespace aether::net::stun::crypto
