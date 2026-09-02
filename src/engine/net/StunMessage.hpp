#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aether::net::stun
{
	// Minimal STUN (RFC 5389 / RFC 8489) Binding client: enough to ask a server "what
	// address and port do you see me coming from", and nothing else.
	//
	// It is deliberately message-only - no sockets. The datagrams go out on the socket
	// ENet already owns, because a NAT mapping belongs to the socket that created it:
	// punching a hole from a second socket would open it for a source port ENet never
	// sends from. Parsing has to live somewhere it can be tested without a network, so
	// it lives here and the socket work sits in the layer above.

	inline constexpr std::uint32_t kMagicCookie = 0x2112A442u;
	inline constexpr std::size_t kHeaderSize = 20;
	inline constexpr std::size_t kTransactionIdSize = 12;

	// Identifies one request/response pair. A response carrying a different id is either
	// stale or someone else's, and answering to it would report the wrong public address.
	struct TransactionId
	{
		std::array<std::uint8_t, kTransactionIdSize> bytes{};

		friend bool operator==(const TransactionId&, const TransactionId&) = default;
	};

	// An IPv4 endpoint in HOST byte order. ENet stores its address in network order, so
	// the conversion happens at that boundary rather than here, where it would be an
	// invisible assumption about who is calling.
	struct Endpoint
	{
		std::uint32_t address = 0;
		std::uint16_t port = 0;

		friend bool operator==(const Endpoint&, const Endpoint&) = default;
	};

	// A transaction id with 96 bits of randomness, which is what makes a response
	// attributable. Uses a seeded PRNG rather than the address of anything: an id that
	// is predictable across runs is an id an off-path attacker can answer first.
	[[nodiscard]] TransactionId MakeTransactionId();

	// The 20-byte Binding Request. There are no attributes: a bare request is all that
	// is needed to learn the reflexive address, and every attribute added is one more
	// thing a server can reject.
	[[nodiscard]] std::array<std::uint8_t, kHeaderSize> BuildBindingRequest(const TransactionId& id);

	// Whether a datagram is plausibly STUN. Needed because these arrive on ENet's own
	// socket: without a cheap check to consume them first, ENet would try to read them
	// as its own protocol. Checks the two leading zero bits and the magic cookie, which
	// is what RFC 5389 says to demultiplex on.
	[[nodiscard]] bool LooksLikeStun(std::span<const std::byte> datagram);

	// What a datagram is, once it is known to be STUN. Connectivity checks between two
	// peers are themselves Binding Requests - the same exchange, aimed at the peer rather
	// than a server - so a punch needs to tell a request from an answer to it.
	enum class MessageKind
	{
		Other,
		BindingRequest,
		BindingSuccess,
	};

	[[nodiscard]] MessageKind Classify(std::span<const std::byte> datagram);

	// The transaction id a datagram carries, so a reply can echo it back.
	[[nodiscard]] std::optional<TransactionId> ReadTransactionId(std::span<const std::byte> datagram);

	inline constexpr std::size_t kBindingResponseSize = kHeaderSize + 12;

	// A Binding Success Response telling the sender the address it reached us from. This
	// is what answers a peer's connectivity check, and it doubles as the proof the hole
	// is open in that direction: receiving one means our own request got through.
	[[nodiscard]] std::array<std::uint8_t, kBindingResponseSize> BuildBindingResponse(const TransactionId& id, const Endpoint& reflexive);

	// The reflexive endpoint from a Binding Success Response, or nullopt if the message
	// is not one, does not match `expected`, or carries no address.
	//
	// Reads XOR-MAPPED-ADDRESS in preference to MAPPED-ADDRESS. The XOR exists precisely
	// because some NATs rewrite anything that looks like an address in a payload, which
	// silently corrupts the plain attribute - so the obfuscated one is the trustworthy
	// answer, and the plain one is only a fallback for servers too old to send it.
	[[nodiscard]] std::optional<Endpoint> ParseBindingResponse(std::span<const std::byte> datagram, const TransactionId& expected);
} // namespace aether::net::stun
