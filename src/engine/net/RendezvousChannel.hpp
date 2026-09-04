#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/NatTraversal.hpp"
#include "net/Signaling.hpp"

namespace aether::net
{
	// Speaks the rendezvous protocol to a server neither peer runs, so two machines
	// that know nothing about each other but that one address can still trade
	// candidates and go on to punch. See ISignalingChannel for why this exists as an
	// interface rather than a concrete choice: a lobby service, a LAN broadcast and
	// this all carry the same handful of endpoints, and the traversal layer above has
	// no business knowing which one is live.
	//
	// This owns a socket of its own, created and bound here and nowhere else. It is
	// NEVER the transport's socket - see the comment at the top of NatTraversal.hpp
	// for why a NAT mapping belongs to the socket that punched from it, and binding a
	// second one for the punch itself would open a hole nothing ever sends from.
	//
	// The wire grammar is fixed by the server this talks to, byte for byte, the same
	// in both directions:
	//   AECR1 <ROOMCODE> <blob>\n
	// where <blob> is exactly what EncodeCandidates() produced. It contains spaces,
	// so it is always the rest of the line, never split further. A trailing CRLF is
	// tolerated as well as a bare LF; nothing else about the shape is negotiable, so
	// the parse and serialise steps below are free functions, deliberately kept apart
	// from socket I/O, so a test can hold them to that grammar without a server or a
	// network anywhere in the loop.

	// One parsed line: which room it claims to be for, and everything after that -
	// still text, because turning it into candidates is DecodeCandidates' job, not
	// this layer's. Keeping the two separate means a malformed candidate list is
	// diagnosed by the one piece of code that already understands that grammar,
	// instead of a second, slightly different copy of its rejection rules living here.
	struct RendezvousDatagram
	{
		std::string roomCode;
		std::string blob;
	};

	// Matches slice C's server-side cap exactly, so the two ends never disagree about
	// what counts as bogus input. A legitimate line is well under 200 bytes even at
	// kMaxCandidates, so anything approaching this bound is not a slow peer, it is
	// malformed or hostile, and is refused outright rather than partially read.
	inline constexpr std::size_t kMaxRendezvousDatagramLength = 1200;

	// The server's own default listening port - one above the 24700 the LAN broadcast
	// channel is fixed to, so both can run on one host during testing. Used whenever
	// a caller supplies a bare host with no ":port".
	inline constexpr std::uint16_t kDefaultRendezvousPort = 24701;

	// The wire form of one line. Used for the client -> server direction, and, since
	// both directions share a grammar, reusable to build the round trip a test needs
	// against ParseRendezvousDatagram below.
	[[nodiscard]] std::string EncodeRendezvousDatagram(std::string_view roomCode, std::string_view blob);

	// Splits a raw datagram into (room code, blob) without judging whether the room
	// code is the one this machine cares about, or whether the blob decodes to
	// anything - both are the caller's business. Refuses anything that does not fully
	// match the grammar: a wrong or absent version, a missing field, more than one
	// line, or a datagram over kMaxRendezvousDatagramLength - salvaging nothing rather
	// than acting on a half-read line.
	[[nodiscard]] std::optional<RendezvousDatagram> ParseRendezvousDatagram(std::string_view datagram);

	// The full read path in one call: parses, checks the room code against the one
	// this channel is joined to, and decodes the blob - returning candidates only
	// when every one of those steps agrees this datagram is real, current, and ours.
	[[nodiscard]] std::optional<CandidateSet> InterpretRendezvousDatagram(std::string_view datagram, std::string_view expectedRoomCode);

	// Folds newly-decoded endpoints into a running total: skips ones already present,
	// stops at kMaxCandidates, and reports whether anything actually grew. Kept apart
	// from socket I/O so the accumulation rule itself - a peer's second datagram adds
	// to the first rather than replacing it, because it may publish its LAN addresses
	// first and its public one only once STUN answers - is testable without two real
	// datagrams ever in flight at once.
	bool AccumulateCandidates(std::vector<NatTraversal::Endpoint>& accumulator, const CandidateSet& incoming);

	class RendezvousChannel final : public ISignalingChannel
	{
	public:
		// `serverAddress` is "host:port", or a bare host to fall back to
		// kDefaultRendezvousPort. `roomCode` is assumed already normalised (see
		// RoomCode.hpp) - this class only ever compares it for equality against what
		// the server echoes back, so it never needs to know that alphabet itself.
		//
		// Resolution and socket setup happen here, once, synchronously: a host that
		// will not resolve is IsUsable() == false with a reason attached, never a
		// crash, and never a retry loop that re-resolves every frame - DNS is asked
		// exactly once, at construction.
		RendezvousChannel(std::string serverAddress, std::string roomCode);
		~RendezvousChannel() override;

		RendezvousChannel(const RendezvousChannel&) = delete;
		RendezvousChannel& operator=(const RendezvousChannel&) = delete;

		// Offers the local candidates to the room. Sent immediately, and re-sent on a
		// timer (see the .cpp) until a datagram comes back from the server for this
		// room or the attempt budget runs out - a single UDP send proves nothing, and
		// the server may not have this room's other peer registered on the first try.
		// A later call replaces the outstanding blob and starts a fresh attempt budget:
		// it carries genuinely new information (the local candidates were known
		// immediately, the public one only once STUN answers) that is worth the same
		// persistence as the first send.
		void Publish(const CandidateSet& candidates) override;

		// Drains every datagram currently queued on the socket, non-blocking. Ignores
		// anything for a different room, anything that fails the grammar, and drives
		// the retransmit timer above. Returns the accumulated candidate set when this
		// call added at least one endpoint that was not already known, nullopt when
		// nothing new arrived.
		[[nodiscard]] std::optional<CandidateSet> Poll() override;

		// False when construction failed - a malformed address, an unresolvable host,
		// or the platform refusing a socket. Publish/Poll are safe no-ops either way;
		// this lets a caller fail the join cleanly instead of waiting out a timeout
		// that could never have succeeded.
		[[nodiscard]] bool IsUsable() const
		{
			return m_usable;
		}

		// Why IsUsable() is false, for a player who has to be told something.
		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_failure;
		}

	private:
		void Fail(std::string reason);
		void Send(const std::string& datagram);
		void MaybeRetransmit();

		// ENet's own ENetSocket type is `SOCKET` on Windows and `int` on POSIX -
		// different widths, different signedness. Storing it as a plain
		// pointer-width integer keeps enet.h, and everything it drags in, out of this
		// header, the same way PortMapping keeps libplum's types out of its own.
		using SocketHandle = std::intptr_t;
		static constexpr SocketHandle kInvalidSocket = -1;

		SocketHandle m_socket = kInvalidSocket;
		bool m_enetAcquired = false;
		bool m_usable = false;
		std::string m_failure;

		std::string m_roomCode;
		// Network byte order for the host, host byte order for the port - ENet's own
		// convention; see NatTraversal::Endpoint for the same split and why.
		std::uint32_t m_serverHost = 0;
		std::uint16_t m_serverPort = 0;

		std::vector<NatTraversal::Endpoint> m_accumulated;

		// The last full candidate set handed to Publish(), kept so it can be re-sent
		// until the server is heard from or the budget below runs out.
		std::optional<std::string> m_outboundBlob;
		bool m_awaitingFirstReply = false;
		std::chrono::steady_clock::time_point m_lastSend{};
		int m_retriesLeft = 0;
	};
} // namespace aether::net
