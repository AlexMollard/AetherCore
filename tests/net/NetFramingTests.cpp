#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "net/NetRpc.hpp"
#include "net/NetSpawn.hpp"

using namespace aether;

// Every packet leads with one NetMessage byte, but WHO writes it differs per kind:
// Spawn/Despawn/Rpc/Welcome are self-framing (their encoder writes it), while
// Snapshot/ScriptFields carry a bare body the sender wraps with FrameMessage. A new
// kind that matches neither convention produces a packet the receive system
// misparses with no error anywhere, so both halves are pinned here - table-driven so
// a kind 7 added without a row is visible as an omission rather than as silence.
namespace
{
	// Mirrors NetworkReceiveSystem::OnData: read data[0] as the kind, hand
	// data.subspan(1) to the decoder.
	net::NetMessage KindOf(std::span<const std::byte> packet)
	{
		return static_cast<net::NetMessage>(static_cast<std::uint8_t>(packet[0]));
	}

	std::vector<std::byte> EncodeWelcome(net::ConnectionId assigned)
	{
		// Mirrors NetworkContext::EncodeWelcome, which lives on the CoreCLR-linked
		// NetworkContext and so cannot be called from EngineTests. The point of the
		// check below is that Welcome is self-framing at all.
		net::ByteWriter w;
		w.U8(static_cast<std::uint8_t>(net::NetMessage::Welcome));
		w.U32(assigned);
		return w.Take();
	}
} // namespace

TEST_CASE("Self-framing encoders lead with their own NetMessage byte")
{
	const std::array<std::pair<net::NetMessage, std::vector<std::byte>>, 4> selfFraming{{
	        {net::NetMessage::Spawn, net::EncodeSpawn(1, 2, "player", {0.f, 0.f, 0.f})},
	        {net::NetMessage::Despawn, net::EncodeDespawn(1)},
	        {net::NetMessage::Rpc, net::EncodeRpc(1, 0xABCDu, 0, net::NetRpcTarget::Server, {})},
	        {net::NetMessage::Welcome, EncodeWelcome(3)},
	}};

	for (const auto& [kind, packet]: selfFraming)
	{
		CAPTURE(static_cast<std::uint32_t>(kind));
		REQUIRE_FALSE(packet.empty());
		CHECK(KindOf(packet) == kind);
	}
}

TEST_CASE("Wrapped payloads are framed byte-for-byte by FrameMessage")
{
	// Snapshot and ScriptFields bodies are opaque here on purpose: the framing
	// contract is "one kind byte, then the body verbatim", independent of what
	// BuildSnapshot/BuildScriptFieldPacket happen to emit.
	const std::vector<std::byte> body{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};

	for (const net::NetMessage kind: {net::NetMessage::Snapshot, net::NetMessage::ScriptFields})
	{
		CAPTURE(static_cast<std::uint32_t>(kind));
		const std::vector<std::byte> framed = net::FrameMessage(kind, body);
		REQUIRE(framed.size() == body.size() + 1);
		CHECK(KindOf(framed) == kind);
		const std::span<const std::byte> payload = std::span<const std::byte>(framed).subspan(1);
		CHECK(std::vector<std::byte>(payload.begin(), payload.end()) == body);
	}
}

TEST_CASE("An empty payload still frames to exactly its kind byte")
{
	const std::vector<std::byte> framed = net::FrameMessage(net::NetMessage::Snapshot, {});
	REQUIRE(framed.size() == 1);
	CHECK(KindOf(framed) == net::NetMessage::Snapshot);
}

TEST_CASE("Every NetMessage kind is covered by one of the two framing conventions")
{
	// The list the two tests above enumerate, spelled out once. A kind added to the
	// enum without being classified fails here rather than silently going untested.
	constexpr std::array<net::NetMessage, 6> kAllKinds{
	        net::NetMessage::Snapshot, net::NetMessage::Spawn, net::NetMessage::Despawn,
	        net::NetMessage::Rpc, net::NetMessage::Welcome, net::NetMessage::ScriptFields,
	};
	constexpr std::array<net::NetMessage, 4> kSelfFraming{
	        net::NetMessage::Spawn, net::NetMessage::Despawn, net::NetMessage::Rpc, net::NetMessage::Welcome,
	};
	constexpr std::array<net::NetMessage, 2> kWrapped{net::NetMessage::Snapshot, net::NetMessage::ScriptFields};

	CHECK(kSelfFraming.size() + kWrapped.size() == kAllKinds.size());
	for (const net::NetMessage kind: kAllKinds)
	{
		CAPTURE(static_cast<std::uint32_t>(kind));
		const bool selfFraming = std::find(kSelfFraming.begin(), kSelfFraming.end(), kind) != kSelfFraming.end();
		const bool wrapped = std::find(kWrapped.begin(), kWrapped.end(), kind) != kWrapped.end();
		CHECK(selfFraming != wrapped); // exactly one, never both, never neither
	}

	// Wire values are part of the protocol; renumbering one silently breaks every
	// build that is not renumbered with it.
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Snapshot) == 1);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::ScriptFields) == 6);
}
