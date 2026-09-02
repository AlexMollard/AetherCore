#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "net/NatTraversal.hpp"

namespace aether::net
{
	// Getting two peers to learn each other's candidate addresses before either can
	// punch. It is a chicken-and-egg problem by nature - they cannot tell each other
	// directly, because being able to do that is the thing being negotiated - so it
	// always goes through something both can already reach.
	//
	// The channel is an interface rather than a concrete backend because the choice is
	// a product decision, not a transport one: a lobby service, a rendezvous server and
	// a LAN broadcast all carry the same handful of endpoints, and the traversal layer
	// below has no business knowing which one is in use.

	// The addresses a peer offers: its public endpoint as a STUN server reported it, and
	// its addresses on its own network. Both matter - two players in one house reach
	// each other over the LAN, where the public endpoints are identical and useless.
	struct CandidateSet
	{
		std::vector<NatTraversal::Endpoint> endpoints;
	};

	// A candidate list is remote input, so the number of them is capped. Without a
	// limit, a peer could name a thousand addresses and have this machine send an
	// unsolicited datagram to every one of them on its say-so.
	inline constexpr std::size_t kMaxCandidates = 8;

	// The wire form: a version, then space-separated `dotted-quad:port`. Text rather
	// than JSON because every channel that carries this has a string field and none of
	// them has a schema, and because the transport layer should not take a JSON
	// dependency to move four numbers.
	[[nodiscard]] std::string EncodeCandidates(const CandidateSet& candidates);

	// Refuses anything it does not fully understand rather than salvaging part of it:
	// a half-read candidate list aims a punch somewhere nobody agreed to.
	[[nodiscard]] std::optional<CandidateSet> DecodeCandidates(const std::string& text);

	class ISignalingChannel
	{
	public:
		virtual ~ISignalingChannel() = default;

		// Offer these candidates to the peer. May be called again as more are learned -
		// the local ones are known immediately, the public one only after STUN answers,
		// and waiting for both before saying anything costs a round trip.
		virtual void Publish(const CandidateSet& candidates) = 0;

		// What the peer has offered since the last call, if anything. Polled rather than
		// delivered by callback so it fits the frame loop the transport is already
		// driven from, and so a backend that can only be asked periodically - a lobby's
		// metadata, say - is not forced to invent an event.
		[[nodiscard]] virtual std::optional<CandidateSet> Poll() = 0;
	};

	// Two channels wired to each other in one process. This is what makes the whole
	// traversal path testable without a network or an account: the punch, the timeouts
	// and the failure diagnostics are all exercised, and only the delivery of a dozen
	// bytes is stubbed.
	class LocalSignalingChannel final : public ISignalingChannel
	{
	public:
		// Connects two ends. Neither owns the other; both must outlive the pairing.
		static void Pair(LocalSignalingChannel& a, LocalSignalingChannel& b);

		void Publish(const CandidateSet& candidates) override;
		[[nodiscard]] std::optional<CandidateSet> Poll() override;

	private:
		LocalSignalingChannel* m_peer = nullptr;
		// Held as encoded text, not as a struct, so the encoding is exercised by every
		// test that uses this rather than only by the ones that test it directly.
		std::optional<std::string> m_inbox;
	};
} // namespace aether::net
