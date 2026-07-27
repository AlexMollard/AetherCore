#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "net/NetInput.hpp"
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

	// ── The coverage tables, and the compile-time promise they make ─────────────
	//
	// Every kind, spelled out once, and the two framing conventions it can belong to.
	// These used to live inside the test case as hand-counted arrays, and that is
	// precisely how NetMessage::Relevancy was added without ever being framed-tested:
	// the enum grew to 7 and the array stayed at 6, so the "a kind added without being
	// classified fails here" comment was describing something the code could not do.
	//
	// Anchored to net::kNetMessageMax and checked by static_assert, it can now. All
	// four failure modes are BUILD errors, not runtime ones - and not test failures
	// that only appear if somebody runs this file:
	//   - a kind added to the enum with no row in kAllKinds
	//   - a kind in kAllKinds classified in neither table, or in both
	//   - a kind listed twice, or the enum renumbered off contiguous-from-1
	//   - a self-framing kind with no actual round-trip row in the test below
	constexpr std::array<net::NetMessage, 8> kAllKinds{
	        net::NetMessage::Snapshot, net::NetMessage::Spawn, net::NetMessage::Despawn,
	        net::NetMessage::Rpc, net::NetMessage::Welcome, net::NetMessage::ScriptFields,
	        net::NetMessage::Relevancy, net::NetMessage::Input,
	};
	constexpr std::array<net::NetMessage, 6> kSelfFraming{
	        net::NetMessage::Spawn, net::NetMessage::Despawn, net::NetMessage::Rpc, net::NetMessage::Welcome,
	        net::NetMessage::Relevancy, net::NetMessage::Input,
	};
	constexpr std::array<net::NetMessage, 2> kWrapped{net::NetMessage::Snapshot, net::NetMessage::ScriptFields};

	// True when `kinds` names every wire value in [1, kNetMessageMax] exactly once.
	template<std::size_t N>
	constexpr bool NamesEveryKindOnce(const std::array<net::NetMessage, N>& kinds)
	{
		for (std::uint8_t value = 1; value <= net::kNetMessageMax; ++value)
		{
			std::size_t seen = 0;
			for (const net::NetMessage kind: kinds)
			{
				if (static_cast<std::uint8_t>(kind) == value)
				{
					++seen;
				}
			}
			if (seen != 1)
			{
				return false;
			}
		}
		return true;
	}

	// True when every defined kind is in exactly one of the two convention tables.
	constexpr bool ClassifiedExactlyOnce()
	{
		for (std::uint8_t value = 1; value <= net::kNetMessageMax; ++value)
		{
			const auto kind = static_cast<net::NetMessage>(value);
			const bool selfFraming = std::find(kSelfFraming.begin(), kSelfFraming.end(), kind) != kSelfFraming.end();
			const bool wrapped = std::find(kWrapped.begin(), kWrapped.end(), kind) != kWrapped.end();
			if (selfFraming == wrapped)
			{
				return false;
			}
		}
		return true;
	}

	static_assert(kAllKinds.size() == net::kNetMessageMax,
	        "A NetMessage kind was added or removed without updating kAllKinds. Add its row, then classify it in "
	        "kSelfFraming or kWrapped and give it a round-trip case below.");
	static_assert(NamesEveryKindOnce(kAllKinds),
	        "kAllKinds must name every wire value in [1, kNetMessageMax] exactly once - a kind is missing, duplicated, "
	        "or the enum is no longer numbered contiguously from 1.");
	static_assert(kSelfFraming.size() + kWrapped.size() == kAllKinds.size(),
	        "Every NetMessage kind belongs to exactly one framing convention.");
	static_assert(ClassifiedExactlyOnce(),
	        "A NetMessage kind is classified in both framing conventions, or in neither.");
} // namespace

TEST_CASE("Self-framing encoders lead with their own NetMessage byte")
{
	// One row per kind in kSelfFraming, and the static_assert below is what keeps it
	// that way: Relevancy was a self-framing kind with no row here at all, so its
	// encoder's leading byte was never checked by anything.
	const std::array<std::pair<net::NetMessage, std::vector<std::byte>>, 6> selfFraming{{
	        {net::NetMessage::Spawn, net::EncodeSpawn(1, 2, "player", {0.f, 0.f, 0.f})},
	        {net::NetMessage::Despawn, net::EncodeDespawn(1)},
	        {net::NetMessage::Rpc, net::EncodeRpc(1, 0xABCDu, 0, net::NetRpcTarget::Server, {})},
	        {net::NetMessage::Welcome, EncodeWelcome(3)},
	        {net::NetMessage::Relevancy, net::EncodeRelevancyLeave(1)},
	        {net::NetMessage::Input, net::EncodeInput(1, 0xABCDu, 0, 1, {})},
	}};
	static_assert(selfFraming.size() == kSelfFraming.size(),
	        "Every self-framing kind needs a round-trip row here, not just a classification.");

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
	// The classification itself is enforced at COMPILE time by the static_asserts at
	// the top of this file - a kind added to the enum without a row, or classified
	// wrongly, never gets as far as producing a test binary. What is left for runtime
	// is the wire values themselves, which the static_asserts deliberately do not pin:
	// they check that the numbering is contiguous from 1, not what any given kind's
	// number IS. Those numbers are the protocol, and renumbering one silently breaks
	// every build that is not renumbered with it.
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Snapshot) == 1);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Spawn) == 2);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Despawn) == 3);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Rpc) == 4);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Welcome) == 5);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::ScriptFields) == 6);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Relevancy) == 7);
	CHECK(static_cast<std::uint8_t>(net::NetMessage::Input) == 8);
	CHECK(net::kNetMessageMax == 8);
}
