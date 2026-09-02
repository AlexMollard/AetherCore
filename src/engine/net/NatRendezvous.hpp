#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/NatTraversal.hpp"
#include "net/Signaling.hpp"

namespace aether::net
{
	// Runs the whole traversal to a conclusion: find out what this socket looks like from
	// outside, tell the peer, hear what the peer looks like, and punch at it.
	//
	// The pieces underneath are each usable alone and each testable alone, which is why
	// they are separate - but nobody wants to hand-drive four state machines in the right
	// order to join a game. This is that order, written once.
	//
	// The ordering matters in a way that is easy to get wrong. Local candidates are
	// published immediately rather than after STUN answers, because two players on one
	// network need nothing from a STUN server and should not wait five seconds for one
	// that may be unreachable. And punching starts the moment the peer's candidates
	// arrive, not once our own discovery finishes: our public endpoint is what the PEER
	// needs in order to punch at us, and is not needed to punch at it.
	class NatRendezvous
	{
	public:
		using Endpoint = NatTraversal::Endpoint;
		using State = NatTraversal::State;

		// Neither reference is owned; both must outlive this. `localPort` is the port the
		// transport is bound to, which is what a peer on the same LAN must be told - the
		// public port a NAT invents is no use to it.
		NatRendezvous(NatTraversal& traversal, ISignalingChannel& channel, std::uint16_t localPort);

		// Publishes local candidates and starts discovery. An empty `stunHost` skips
		// discovery entirely, which is the LAN case: there is no NAT to ask about.
		void Begin(const std::string& stunHost, std::uint16_t stunPort = 3478);

		// Drives both halves. Call once per frame with real seconds.
		void Tick(float deltaSeconds);

		[[nodiscard]] State GetState() const;
		[[nodiscard]] std::optional<Endpoint> OpenPath() const;
		[[nodiscard]] const std::string& FailureReason() const;

	private:
		void PublishKnownCandidates();

		NatTraversal& m_traversal;
		ISignalingChannel& m_channel;
		std::uint16_t m_localPort = 0;

		bool m_begun = false;
		bool m_publishedPublic = false;
		// Separates "discovery gave up" from "the punch gave up". Only the latter ends
		// the attempt; the former just means one candidate fewer to offer.
		bool m_punchStarted = false;
		// Accumulated rather than replaced: a peer may publish twice - once with its LAN
		// addresses and again once STUN has answered it - and the second message must add
		// to the first rather than discard candidates that might have been the ones to
		// work.
		std::vector<Endpoint> m_peerCandidates;

		float m_elapsed = 0.0f;
		std::string m_failure;
		bool m_timedOut = false;
	};
} // namespace aether::net
