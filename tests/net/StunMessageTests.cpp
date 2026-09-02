#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "net/StunMessage.hpp"

using namespace aether::net::stun;

namespace
{
	std::span<const std::byte> AsBytes(const std::vector<std::uint8_t>& v)
	{
		return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
	}

	void AppendU16(std::vector<std::uint8_t>& v, std::uint16_t x)
	{
		v.push_back(static_cast<std::uint8_t>(x >> 8));
		v.push_back(static_cast<std::uint8_t>(x & 0xFF));
	}

	void AppendU32(std::vector<std::uint8_t>& v, std::uint32_t x)
	{
		v.push_back(static_cast<std::uint8_t>(x >> 24));
		v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
		v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
		v.push_back(static_cast<std::uint8_t>(x & 0xFF));
	}

	// A Binding Success Response carrying the given attributes.
	std::vector<std::uint8_t> MakeResponse(const TransactionId& id, const std::vector<std::uint8_t>& attributes, std::uint16_t type = 0x0101)
	{
		std::vector<std::uint8_t> out;
		AppendU16(out, type);
		AppendU16(out, static_cast<std::uint16_t>(attributes.size()));
		AppendU32(out, kMagicCookie);
		out.insert(out.end(), id.bytes.begin(), id.bytes.end());
		out.insert(out.end(), attributes.begin(), attributes.end());
		return out;
	}

	// One address attribute: reserved, family, port, address - values written raw, so a
	// test decides for itself whether they are XORed.
	std::vector<std::uint8_t> AddressAttribute(std::uint16_t type, std::uint16_t port, std::uint32_t address)
	{
		std::vector<std::uint8_t> a;
		AppendU16(a, type);
		AppendU16(a, 8);
		a.push_back(0x00);
		a.push_back(0x01); // IPv4
		AppendU16(a, port);
		AppendU32(a, address);
		return a;
	}

	constexpr std::uint32_t kAddress = 0xC0A80164; // 192.168.1.100
	constexpr std::uint16_t kPort = 54321;         // 0xD431
	// Hand-computed: address ^ 0x2112A442 and port ^ 0x2112. If the implementation ever
	// stops XORing, or XORs with the wrong half of the cookie, these stop matching.
	constexpr std::uint32_t kXorAddress = 0xE1BAA526;
	constexpr std::uint16_t kXorPort = 0xF523;
} // namespace

TEST_CASE("A binding request is a bare, well-formed STUN header")
{
	const TransactionId id = MakeTransactionId();
	const auto request = BuildBindingRequest(id);

	CHECK(request.size() == kHeaderSize);
	CHECK(request[0] == 0x00);
	CHECK(request[1] == 0x01);          // Binding Request
	CHECK(request[2] == 0x00);
	CHECK(request[3] == 0x00);          // no attributes
	CHECK(request[4] == 0x21);          // magic cookie, big-endian
	CHECK(request[5] == 0x12);
	CHECK(request[6] == 0xA4);
	CHECK(request[7] == 0x42);
	for (std::size_t i = 0; i < kTransactionIdSize; ++i)
	{
		CHECK(request[8 + i] == id.bytes[i]);
	}

	const std::vector<std::uint8_t> copy(request.begin(), request.end());
	CHECK(LooksLikeStun(AsBytes(copy)));
}

TEST_CASE("Transaction ids differ between requests")
{
	// Two identical ids would make responses unattributable, which is the property the
	// parser relies on to reject someone else's answer.
	CHECK_FALSE(MakeTransactionId() == MakeTransactionId());
}

TEST_CASE("XOR-MAPPED-ADDRESS decodes to the reflexive endpoint")
{
	const TransactionId id = MakeTransactionId();
	const auto response = MakeResponse(id, AddressAttribute(0x0020, kXorPort, kXorAddress));

	const auto endpoint = ParseBindingResponse(AsBytes(response), id);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->address == kAddress);
	CHECK(endpoint->port == kPort);
}

TEST_CASE("A response for a different transaction is refused")
{
	// The whole point of the id: a stale or spoofed response would otherwise be reported
	// as this agent's public address, and every later punch would aim at the wrong place.
	const TransactionId mine = MakeTransactionId();
	const TransactionId theirs = MakeTransactionId();
	const auto response = MakeResponse(theirs, AddressAttribute(0x0020, kXorPort, kXorAddress));

	CHECK_FALSE(ParseBindingResponse(AsBytes(response), mine).has_value());
}

TEST_CASE("XOR-MAPPED-ADDRESS wins over a contradicting MAPPED-ADDRESS")
{
	// A NAT that rewrites addresses it finds in payloads corrupts the plain attribute
	// and cannot touch the obfuscated one, so the plain value here is deliberately wrong.
	const TransactionId id = MakeTransactionId();
	std::vector<std::uint8_t> attributes = AddressAttribute(0x0001, 9999, 0x0A000001);
	const auto xorAttribute = AddressAttribute(0x0020, kXorPort, kXorAddress);
	attributes.insert(attributes.end(), xorAttribute.begin(), xorAttribute.end());

	const auto endpoint = ParseBindingResponse(AsBytes(MakeResponse(id, attributes)), id);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->address == kAddress);
	CHECK(endpoint->port == kPort);
}

TEST_CASE("MAPPED-ADDRESS alone is still read, for servers that send nothing else")
{
	const TransactionId id = MakeTransactionId();
	const auto response = MakeResponse(id, AddressAttribute(0x0001, kPort, kAddress));

	const auto endpoint = ParseBindingResponse(AsBytes(response), id);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->address == kAddress);
	CHECK(endpoint->port == kPort);
}

TEST_CASE("An attribute whose length is not a multiple of four does not desync the walk")
{
	// Real servers put SOFTWARE in front of the address. Its value is rarely a multiple
	// of 4, and the padding is not counted in the length - stepping by the length alone
	// lands mid-attribute and loses the address entirely.
	const TransactionId id = MakeTransactionId();
	std::vector<std::uint8_t> attributes;
	AppendU16(attributes, 0x8022); // SOFTWARE
	AppendU16(attributes, 5);
	for (const char c: {'a', 'b', 'c', 'd', 'e'})
	{
		attributes.push_back(static_cast<std::uint8_t>(c));
	}
	attributes.insert(attributes.end(), 3, 0); // padding to the 4-byte boundary
	const auto xorAttribute = AddressAttribute(0x0020, kXorPort, kXorAddress);
	attributes.insert(attributes.end(), xorAttribute.begin(), xorAttribute.end());

	const auto endpoint = ParseBindingResponse(AsBytes(MakeResponse(id, attributes)), id);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->address == kAddress);
	CHECK(endpoint->port == kPort);
}

TEST_CASE("Malformed and non-STUN datagrams are refused rather than read past")
{
	const TransactionId id = MakeTransactionId();

	// An ENet datagram sharing the socket must not be mistaken for STUN.
	const std::vector<std::uint8_t> enetish(40, 0xFF);
	CHECK_FALSE(LooksLikeStun(AsBytes(enetish)));
	CHECK_FALSE(ParseBindingResponse(AsBytes(enetish), id).has_value());

	// Too short to hold a header.
	const std::vector<std::uint8_t> stub{0x00, 0x01, 0x00, 0x00};
	CHECK_FALSE(LooksLikeStun(AsBytes(stub)));

	// Right shape, wrong cookie.
	auto badCookie = MakeResponse(id, AddressAttribute(0x0020, kXorPort, kXorAddress));
	badCookie[4] = 0x00;
	CHECK_FALSE(LooksLikeStun(AsBytes(badCookie)));

	// An error response is STUN, but carries no reflexive address to believe.
	const auto errorResponse = MakeResponse(id, {}, 0x0111);
	CHECK(LooksLikeStun(AsBytes(errorResponse)));
	CHECK_FALSE(ParseBindingResponse(AsBytes(errorResponse), id).has_value());

	// An attribute claiming more bytes than the datagram holds must stop the walk, not
	// read off the end of it.
	std::vector<std::uint8_t> truncated;
	AppendU16(truncated, 0x0020);
	AppendU16(truncated, 64); // lies about its length
	truncated.push_back(0x00);
	truncated.push_back(0x01);
	CHECK_FALSE(ParseBindingResponse(AsBytes(MakeResponse(id, truncated)), id).has_value());
}

TEST_CASE("A binding response carries the peer's address back to it")
{
	const TransactionId id = MakeTransactionId();
	const auto response = BuildBindingResponse(id, Endpoint{kAddress, kPort});
	const std::vector<std::uint8_t> bytes(response.begin(), response.end());

	CHECK(Classify(AsBytes(bytes)) == MessageKind::BindingSuccess);

	// Asserted against the hand-computed vector, not just round-tripped: a build and a
	// parse that XOR with the same wrong value agree with each other perfectly.
	CHECK(bytes[20] == 0x00);
	CHECK(bytes[21] == 0x20); // XOR-MAPPED-ADDRESS
	CHECK(bytes[25] == 0x01); // IPv4
	CHECK(bytes[26] == static_cast<std::uint8_t>(kXorPort >> 8));
	CHECK(bytes[27] == static_cast<std::uint8_t>(kXorPort & 0xFF));
	CHECK(bytes[28] == static_cast<std::uint8_t>(kXorAddress >> 24));
	CHECK(bytes[31] == static_cast<std::uint8_t>(kXorAddress & 0xFF));

	const auto parsed = ParseBindingResponse(AsBytes(bytes), id);
	REQUIRE(parsed.has_value());
	CHECK(parsed->address == kAddress);
	CHECK(parsed->port == kPort);
}

TEST_CASE("A connectivity check is told apart from the answer to one")
{
	// The punch sends Binding Requests at the peer and must reply to the peer's own,
	// so confusing the two directions would leave both sides waiting for each other.
	const TransactionId id = MakeTransactionId();
	const auto request = BuildBindingRequest(id);
	const std::vector<std::uint8_t> requestBytes(request.begin(), request.end());
	const auto response = BuildBindingResponse(id, Endpoint{kAddress, kPort});
	const std::vector<std::uint8_t> responseBytes(response.begin(), response.end());

	CHECK(Classify(AsBytes(requestBytes)) == MessageKind::BindingRequest);
	CHECK(Classify(AsBytes(responseBytes)) == MessageKind::BindingSuccess);

	const std::vector<std::uint8_t> notStun(40, 0xFF);
	CHECK(Classify(AsBytes(notStun)) == MessageKind::Other);

	// A reply has to echo the id it answers, so it must be readable off the request.
	const auto echoed = ReadTransactionId(AsBytes(requestBytes));
	REQUIRE(echoed.has_value());
	CHECK(*echoed == id);
	CHECK_FALSE(ReadTransactionId(AsBytes(notStun)).has_value());
}
