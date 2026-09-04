#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

#include "net/StunAttributes.hpp"
#include "net/StunAuth.hpp"
#include "net/StunMessage.hpp"

using namespace aether::net::stun;

namespace
{
	std::span<const std::byte> AsBytes(const std::vector<std::uint8_t>& v)
	{
		return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
	}

	TransactionId MakeId(std::array<std::uint8_t, kTransactionIdSize> bytes)
	{
		TransactionId id;
		id.bytes = bytes;
		return id;
	}

	// RFC 5769 s2.1: a bare Binding Request with a short-term (ICE) credential. PRIORITY
	// (0x0024) and ICE-CONTROLLED (0x8029) are ICE attributes this layer has no named
	// enumerator for; they sit here specifically to prove the TLV walk skips an
	// attribute it does not recognize - including one whose length is not a multiple of
	// four - without losing its place before USERNAME, MESSAGE-INTEGRITY, and
	// FINGERPRINT.
	const std::vector<std::uint8_t> kSampleRequest = {
		0x00, 0x01, 0x00, 0x58, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
		0x80, 0x22, 0x00, 0x10, 0x53, 0x54, 0x55, 0x4e, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x63, 0x6c, 0x69, 0x65, 0x6e, 0x74,
		0x00, 0x24, 0x00, 0x04, 0x6e, 0x00, 0x01, 0xff,
		0x80, 0x29, 0x00, 0x08, 0x93, 0x2f, 0xf9, 0xb1, 0x51, 0x26, 0x3b, 0x36,
		0x00, 0x06, 0x00, 0x09, 0x65, 0x76, 0x74, 0x6a, 0x3a, 0x68, 0x36, 0x76, 0x59, 0x20, 0x20, 0x20,
		0x00, 0x08, 0x00, 0x14, 0x9a, 0xea, 0xa7, 0x0c, 0xbf, 0xd8, 0xcb, 0x56, 0x78, 0x1e, 0xf2, 0xb5, 0xb2, 0xd3, 0xf2, 0x49, 0xc1, 0xb5, 0x71, 0xa2,
		0x80, 0x28, 0x00, 0x04, 0xe5, 0x7a, 0x3b, 0xcf,
	};

	// RFC 5769 s2.2: an IPv4 Binding Success Response carrying XOR-MAPPED-ADDRESS.
	const std::vector<std::uint8_t> kSampleIpv4Response = {
		0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
		0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
		0x00, 0x20, 0x00, 0x08, 0x00, 0x01, 0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43,
		0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99, 0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b, 0xe7, 0xd7,
		0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96,
	};

	// RFC 5769 s2.3: the same response, but with an IPv6 mapped address. Used only to
	// prove XorAddress refuses a non-IPv4 family byte rather than misreading it.
	const std::vector<std::uint8_t> kSampleIpv6Response = {
		0x01, 0x01, 0x00, 0x48, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
		0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74, 0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20,
		0x00, 0x20, 0x00, 0x14, 0x00, 0x02, 0xa1, 0x47, 0x01, 0x13, 0xa9, 0xfa, 0xa5, 0xd3, 0xf1, 0x79, 0xbc, 0x25, 0xf4, 0xb5, 0xbe, 0xd2, 0xb9, 0xd9,
		0x00, 0x08, 0x00, 0x14, 0xa3, 0x82, 0x95, 0x4e, 0x4b, 0xe6, 0x7b, 0xf1, 0x17, 0x84, 0xc9, 0x7c, 0x82, 0x92, 0xc2, 0x75, 0xbf, 0xe3, 0xed, 0x41,
		0x80, 0x28, 0x00, 0x04, 0xc8, 0xfb, 0x0b, 0x4c,
	};

	// RFC 5769 s2.4: a Binding Request authenticated with a long-term credential -
	// USERNAME (UTF-8), REALM, NONCE, and MESSAGE-INTEGRITY keyed by
	// MD5(username ":" realm ":" password). This is also the one sample message padded
	// with NUL bytes rather than spaces, so it is the vector used to prove the builder
	// reproduces a sample byte for byte.
	const std::vector<std::uint8_t> kSampleLongTermRequest = {
		0x00, 0x01, 0x00, 0x60, 0x21, 0x12, 0xa4, 0x42, 0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e,
		0x00, 0x06, 0x00, 0x12, 0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3, 0x83, 0x83, 0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xb9, 0x00, 0x00,
		0x00, 0x15, 0x00, 0x1c, 0x66, 0x2f, 0x2f, 0x34, 0x39, 0x39, 0x6b, 0x39, 0x35, 0x34, 0x64, 0x36, 0x4f, 0x4c, 0x33, 0x34, 0x6f, 0x4c, 0x39, 0x46, 0x53, 0x54, 0x76, 0x79, 0x36, 0x34, 0x73, 0x41,
		0x00, 0x14, 0x00, 0x0b, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x6f, 0x72, 0x67, 0x00,
		0x00, 0x08, 0x00, 0x14, 0xf6, 0x70, 0x24, 0x65, 0x6d, 0xd6, 0x4a, 0x3e, 0x02, 0xb8, 0xe0, 0x71, 0x2e, 0x85, 0xc9, 0xa2, 0x8c, 0xa8, 0x96, 0x66,
	};

	// The RFC's Japanese katakana username ("matrix"), expressed as raw UTF-8 bytes
	// rather than a source-literal string to sidestep any question of the compiler's
	// assumed source encoding.
	constexpr std::uint8_t kUsernameUtf8[] = {0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x88, 0xe3, 0x83, 0xaa, 0xe3, 0x83, 0x83, 0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xb9};

	std::string_view UsernameText()
	{
		return {reinterpret_cast<const char*>(kUsernameUtf8), sizeof(kUsernameUtf8)};
	}

	// Recomputes RFC 5389's FINGERPRINT independently of MessageReader, over the exact
	// prefix a real sender would have hashed (FINGERPRINT is always the last attribute,
	// so no length-field patching is needed here the way MESSAGE-INTEGRITY needs).
	std::uint32_t ExpectedFingerprint(const std::vector<std::uint8_t>& message)
	{
		const auto prefix = AsBytes(message).first(message.size() - 8);
		return crypto::Crc32(prefix) ^ 0x5354554Eu;
	}
} // namespace

TEST_CASE("The message type field survives the round trip for methods that need interleaved class bits")
{
	// Every method this layer defines fits in 4 bits, which is exactly the case where a
	// naive "method in the low bits, class shifted above it" encoding happens to work
	// for a Request (class 0) and silently produces the wrong bits otherwise - so this
	// pins two non-Binding, non-Request combinations against hand-derived RFC 5389 s6
	// wire values, not just round-trips them.
	const TransactionId id = MakeId({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});

	{
		MessageBuilder builder(Method::Allocate, MessageClass::ErrorResponse, id);
		const auto bytes = builder.Bytes();
		CHECK(bytes[0] == 0x01);
		CHECK(bytes[1] == 0x13); // Allocate=0x003, ErrorResponse=0b11 -> 0x0113

		const auto reader = MessageReader::Parse(std::as_bytes(bytes));
		REQUIRE(reader.has_value());
		CHECK(reader->GetMethod() == Method::Allocate);
		CHECK(reader->GetClass() == MessageClass::ErrorResponse);
	}
	{
		MessageBuilder builder(Method::ChannelBind, MessageClass::SuccessResponse, id);
		const auto bytes = builder.Bytes();
		CHECK(bytes[0] == 0x01);
		CHECK(bytes[1] == 0x09); // ChannelBind=0x009, SuccessResponse=0b10 -> 0x0109

		const auto reader = MessageReader::Parse(std::as_bytes(bytes));
		REQUIRE(reader.has_value());
		CHECK(reader->GetMethod() == Method::ChannelBind);
		CHECK(reader->GetClass() == MessageClass::SuccessResponse);
	}
}

TEST_CASE("A built XOR-MAPPED-ADDRESS round-trips through the reader")
{
	const TransactionId id = MakeId({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
	const Endpoint endpoint{0xC0A80164u, 54321}; // 192.168.1.100:54321

	MessageBuilder builder(Method::Binding, MessageClass::SuccessResponse, id);
	builder.AddXorAddress(Attribute::XorMappedAddress, endpoint);

	const auto reader = MessageReader::Parse(std::as_bytes(builder.Bytes()));
	REQUIRE(reader.has_value());
	const auto decoded = reader->XorAddress(Attribute::XorMappedAddress);
	REQUIRE(decoded.has_value());
	CHECK(decoded->address == endpoint.address);
	CHECK(decoded->port == endpoint.port);
}

TEST_CASE("RFC 5769 s2.1: reading the sample request recovers its attributes despite unrecognized ones between them")
{
	const auto reader = MessageReader::Parse(AsBytes(kSampleRequest));
	REQUIRE(reader.has_value());
	CHECK(reader->GetMethod() == Method::Binding);
	CHECK(reader->GetClass() == MessageClass::Request);

	const TransactionId expectedId = MakeId({0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae});
	CHECK(reader->GetTransactionId() == expectedId);

	const auto software = reader->Text(Attribute::Software);
	REQUIRE(software.has_value());
	CHECK(*software == std::string_view("STUN test client"));
	// USERNAME's 3 padding bytes (ASCII spaces here) must not leak into the text.
	const auto trimmedUsername = reader->Text(Attribute::Username);
	REQUIRE(trimmedUsername.has_value());
	CHECK(*trimmedUsername == std::string_view("evtj:h6vY"));

	const auto integrity = reader->Find(Attribute::MessageIntegrity);
	REQUIRE(integrity.has_value());
	CHECK(integrity->size() == 20);

	// Cross-checks slice A's Crc32 against the RFC vector via the reader's own TLV walk.
	const auto fingerprint = reader->Find(Attribute::Fingerprint);
	REQUIRE(fingerprint.has_value());
	REQUIRE(fingerprint->size() == 4);
	const auto* fp = reinterpret_cast<const std::uint8_t*>(fingerprint->data());
	const std::uint32_t actualFingerprint = (static_cast<std::uint32_t>(fp[0]) << 24) | (static_cast<std::uint32_t>(fp[1]) << 16) | (static_cast<std::uint32_t>(fp[2]) << 8) | fp[3];
	CHECK(actualFingerprint == ExpectedFingerprint(kSampleRequest));
}

TEST_CASE("RFC 5769 s2.2: reading the sample IPv4 response recovers XOR-MAPPED-ADDRESS")
{
	const auto reader = MessageReader::Parse(AsBytes(kSampleIpv4Response));
	REQUIRE(reader.has_value());
	CHECK(reader->GetMethod() == Method::Binding);
	CHECK(reader->GetClass() == MessageClass::SuccessResponse);
	const auto software = reader->Text(Attribute::Software);
	REQUIRE(software.has_value());
	CHECK(*software == std::string_view("test vector"));

	const auto endpoint = reader->XorAddress(Attribute::XorMappedAddress);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->address == 0xC0000201u); // 192.0.2.1
	CHECK(endpoint->port == 32853);

	const auto fingerprint = reader->Find(Attribute::Fingerprint);
	REQUIRE(fingerprint.has_value());
	REQUIRE(fingerprint->size() == 4);
	const auto* fp = reinterpret_cast<const std::uint8_t*>(fingerprint->data());
	const std::uint32_t actualFingerprint = (static_cast<std::uint32_t>(fp[0]) << 24) | (static_cast<std::uint32_t>(fp[1]) << 16) | (static_cast<std::uint32_t>(fp[2]) << 8) | fp[3];
	CHECK(actualFingerprint == ExpectedFingerprint(kSampleIpv4Response));
}

TEST_CASE("RFC 5769 s2.3: an IPv6 mapped address is refused, not misread as a truncated IPv4 one")
{
	// ENet, and this whole subsystem, only ever has a 32-bit address to compare
	// against; an IPv6 family byte must be rejected outright rather than have its first
	// four address bytes silently reinterpreted as an IPv4 one.
	const auto reader = MessageReader::Parse(AsBytes(kSampleIpv6Response));
	REQUIRE(reader.has_value());
	CHECK_FALSE(reader->XorAddress(Attribute::XorMappedAddress).has_value());
}

TEST_CASE("RFC 5769 s2.4: reading the long-term-authenticated request recovers username, realm, nonce, and integrity")
{
	const auto reader = MessageReader::Parse(AsBytes(kSampleLongTermRequest));
	REQUIRE(reader.has_value());
	CHECK(reader->GetMethod() == Method::Binding);
	CHECK(reader->GetClass() == MessageClass::Request);

	const TransactionId expectedId = MakeId({0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e});
	CHECK(reader->GetTransactionId() == expectedId);

	const auto username = reader->Text(Attribute::Username);
	REQUIRE(username.has_value());
	CHECK(*username == UsernameText());
	const auto realm = reader->Text(Attribute::Realm);
	REQUIRE(realm.has_value());
	CHECK(*realm == std::string_view("example.org"));
	const auto nonce = reader->Text(Attribute::Nonce);
	REQUIRE(nonce.has_value());
	CHECK(*nonce == std::string_view("f//499k954d6OL34oL9FSTvy64sA"));

	// Long-term key derivation from slice A, then MESSAGE-INTEGRITY over the message as
	// it stood before this attribute was appended.
	const crypto::Md5Digest key = crypto::LongTermKey(*username, "example.org", "TheMatrIX");
	CHECK(reader->VerifyMessageIntegrity(key));

	const crypto::Md5Digest wrongKey = crypto::LongTermKey(*username, "example.org", "WrongPassword");
	CHECK_FALSE(reader->VerifyMessageIntegrity(wrongKey));
}

TEST_CASE("RFC 5769 s2.4: building the same request reproduces it byte for byte")
{
	// The one sample message padded with NUL bytes (per RFC 5769 s2's introduction),
	// which is exactly what this builder writes - so, unlike the other three samples
	// (space-padded), this one can be checked for an exact byte match rather than only
	// round-tripped through the reader.
	const TransactionId id = MakeId({0x78, 0xad, 0x34, 0x33, 0xc6, 0xad, 0x72, 0xc0, 0x29, 0xda, 0x41, 0x2e});
	const std::string_view username = UsernameText();

	MessageBuilder builder(Method::Binding, MessageClass::Request, id);
	builder.AddText(Attribute::Username, username);
	builder.AddText(Attribute::Nonce, "f//499k954d6OL34oL9FSTvy64sA");
	builder.AddText(Attribute::Realm, "example.org");

	const crypto::Md5Digest key = crypto::LongTermKey(username, "example.org", "TheMatrIX");
	builder.AppendMessageIntegrity(key);

	const auto bytes = builder.Bytes();
	REQUIRE(bytes.size() == kSampleLongTermRequest.size());
	CHECK(std::equal(bytes.begin(), bytes.end(), kSampleLongTermRequest.begin()));
}

TEST_CASE("A truncated header is refused")
{
	const std::vector<std::uint8_t> stub{0x00, 0x01, 0x00, 0x00, 0x21, 0x12};
	CHECK_FALSE(MessageReader::Parse(AsBytes(stub)).has_value());
}

TEST_CASE("A declared message length longer than the buffer is refused")
{
	// The header claims 64 bytes of attributes but the datagram holds none.
	std::vector<std::uint8_t> message = kSampleRequest;
	message.resize(20); // strip every attribute, but leave the original length field
	message[2] = 0x00;
	message[3] = 0x40; // 64
	CHECK_FALSE(MessageReader::Parse(AsBytes(message)).has_value());
}

TEST_CASE("An attribute claiming a length past the declared end is refused, not read out of bounds")
{
	std::vector<std::uint8_t> message = {
		0x00, 0x01, 0x00, 0x08, // header, 8 bytes of attributes declared
		0x21, 0x12, 0xa4, 0x42,
		0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
		0x00, 0x20, 0x00, 0x40, // XOR-MAPPED-ADDRESS claiming a 64-byte value
		0x00, 0x01, 0x00, 0x00, // only 4 bytes actually follow
	};
	CHECK_FALSE(MessageReader::Parse(AsBytes(message)).has_value());
}

TEST_CASE("A zero-length attribute parses without desyncing the walk")
{
	std::vector<std::uint8_t> message = {
		0x00, 0x01, 0x00, 0x04, // header, 4 bytes of attributes declared
		0x21, 0x12, 0xa4, 0x42,
		0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
		0x00, 0x0a, 0x00, 0x00, // UNKNOWN-ATTRIBUTES, zero-length
	};
	const auto reader = MessageReader::Parse(AsBytes(message));
	REQUIRE(reader.has_value());
	const auto value = reader->Find(Attribute::UnknownAttributes);
	REQUIRE(value.has_value());
	CHECK(value->empty());
}

TEST_CASE("VerifyMessageIntegrity is safe when the attribute is absent, truncated, or the wrong size")
{
	const TransactionId id = MakeId({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
	const crypto::Md5Digest key = crypto::LongTermKey("user", "realm", "pw");

	{
		// No MESSAGE-INTEGRITY at all.
		MessageBuilder builder(Method::Binding, MessageClass::Request, id);
		builder.AddText(Attribute::Username, "user");
		const auto reader = MessageReader::Parse(std::as_bytes(builder.Bytes()));
		REQUIRE(reader.has_value());
		CHECK_FALSE(reader->VerifyMessageIntegrity(key));
	}
	{
		// A MESSAGE-INTEGRITY-shaped attribute that is not 20 bytes.
		std::vector<std::uint8_t> message = {
			0x00, 0x01, 0x00, 0x08,
			0x21, 0x12, 0xa4, 0x42,
			0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae,
			0x00, 0x08, 0x00, 0x04, 0xde, 0xad, 0xbe, 0xef,
		};
		const auto reader = MessageReader::Parse(AsBytes(message));
		REQUIRE(reader.has_value());
		CHECK_FALSE(reader->VerifyMessageIntegrity(key));
	}
}

TEST_CASE("ChannelData is told apart from a STUN message by its leading bits")
{
	// RFC 5766 s11: channel numbers occupy 0x4000-0x7FFF, whose top two bits are
	// `01`; a STUN message's are always `00`. This is the only demux the two share a
	// socket with, so getting it backwards would hand ChannelData payloads to
	// MessageReader::Parse or vice versa.
	const std::vector<std::uint8_t> channelData = {0x40, 0x00, 0x00, 0x04, 0xde, 0xad, 0xbe, 0xef};
	CHECK(LooksLikeChannelData(AsBytes(channelData)));
	CHECK_FALSE(MessageReader::Parse(AsBytes(channelData)).has_value());

	const TransactionId id = MakeId({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
	const MessageBuilder builder(Method::Binding, MessageClass::Request, id);
	const auto bindingBytes = std::as_bytes(builder.Bytes());
	CHECK_FALSE(LooksLikeChannelData(bindingBytes));

	// Too short to be either.
	const std::vector<std::uint8_t> tooShort = {0x40, 0x00, 0x00};
	CHECK_FALSE(LooksLikeChannelData(AsBytes(tooShort)));
}
