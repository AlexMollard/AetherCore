#include "net/NetTraversalSession.hpp"

#include <algorithm>
#include <utility>

#include "net/BroadcastSignaling.hpp"
#include "net/RendezvousChannel.hpp"
#include "net/RoomCode.hpp"
#include "net/TurnClient.hpp"
#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// How long to let the router answer before assuming it will not. This cannot
		// simply wait for PortMapping's own resolution: libplum's worker runs on real
		// wall-clock time with no caller-visible bound (see the "real router" test in
		// PortMappingTests.cpp, which allows it up to fifteen seconds), and a punch
		// fallback sitting ready to go must not be held hostage to however long that
		// takes on a router that will refuse anyway. Measured in the SIMULATED
		// seconds Tick() is called with, same as every other timeout in this file, so
		// a test can cross it in one call with no real waiting.
		constexpr float kMappingTimeoutSeconds = 5.0f;

		// How long to wait for ENet's own handshake once a path has opened. A punch
		// proves the hole exists; it says nothing about whether the CONNECT command
		// that follows it actually arrives, and a hole that closed in the interim
		// must not leave this sitting in Connecting forever.
		constexpr float kConnectingTimeoutSeconds = 10.0f;

		// How long to give a relay before deciding it never produced a path either -
		// generous, because TurnClient's own Allocate give-up (kMaxSendsPerTransaction
		// retries at RFC 5389 s7.2.1 backoff) alone can take upward of a minute against a
		// server that is simply not answering, and this budget has to outlast that AND
		// leave the peer a real chance to publish its own relayed candidate afterward.
		constexpr float kRelayTimeoutSeconds = 90.0f;

		// A joiner needs a KNOWN local port: NatTraversal::LocalCandidates and
		// NatRendezvous both take the bound port as a caller-supplied number rather
		// than reading it back off the socket, so an ephemeral bind (port 0) would
		// publish a LAN candidate at port 0 - one nobody could ever reach.
		// NetworkSubsystem exposes no accessor for whatever port an ephemeral bind
		// actually received, so this tries a small fixed range instead of a single
		// literal port. It does not eliminate two joiners on ONE machine racing for
		// the same port, only make it unlikely.
		//
		// ponytail: the real fix is a NetworkSubsystem::LocalPort() reading the ENet
		// host's own address back after an ephemeral bind, which would let this go
		// back to port 0 like every other client socket in this codebase. Out of
		// scope for this batch - NetworkSubsystem is not a file this change owns.
		constexpr std::uint16_t kJoinerPortRangeStart = 24740;
		constexpr int kJoinerPortAttempts = 8;
	} // namespace

	NetTraversalSession::NetTraversalSession(NetworkSubsystem& transport)
	      : m_transport(transport)
	{
	}

	void NetTraversalSession::ConfigureSignaling(SignalingBackend backend, std::string address)
	{
		m_backend = backend;
		m_signalingAddress = std::move(address);
		m_useInjectedChannel = false;
		m_ownedSignaling.reset();
		m_signaling = nullptr;
	}

	void NetTraversalSession::ConfigureSignalingChannel(ISignalingChannel& channel)
	{
		m_ownedSignaling.reset();
		m_signaling = &channel;
		m_useInjectedChannel = true;
	}

	void NetTraversalSession::SetStunServer(std::string host, std::uint16_t port)
	{
		m_stunHost = std::move(host);
		m_stunPort = port;
	}

	void NetTraversalSession::SetTurnServer(std::string host, std::uint16_t port, std::string username, std::string password, bool allowRelay)
	{
		m_turnHost = std::move(host);
		m_turnPort = port;
		m_turnUsername = std::move(username);
		m_turnPassword = std::move(password);
		m_allowRelay = allowRelay;
	}

	void NetTraversalSession::ResetForNewAttempt()
	{
		// A caller may retry straight from Failed, and this class is driven directly
		// by tests with no NetworkContext-level guard above it - so every attempt
		// starts by unconditionally letting go of whatever the last one left behind
		// rather than trusting it is already clean.
		m_transport.Disconnect();
		m_portMapping.Release();
		m_rendezvous.reset();

		m_state = TraversalState::Idle;
		m_failure.clear();
		m_mappingElapsed = 0.0f;
		m_connectingElapsed = 0.0f;
		m_relayPublished = false;
		m_relayElapsed = 0.0f;
		m_joinInProgress = false;
		m_localPort = 0;
	}

	bool NetTraversalSession::EnsureSignalingChannel(const std::string& roomCode)
	{
		if (m_useInjectedChannel)
		{
			// A test (or a future hosted backend) already wired one up - see
			// ConfigureSignalingChannel.
			return true;
		}

		std::unique_ptr<ISignalingChannel> channel;
		if (m_backend == SignalingBackend::LanBroadcast)
		{
			auto broadcast = std::make_unique<BroadcastSignalingChannel>(roomCode);
			if (!broadcast->IsUsable())
			{
				Fail(broadcast->FailureReason());
				return false;
			}
			channel = std::move(broadcast);
		}
		else
		{
			auto rendezvous = std::make_unique<RendezvousChannel>(m_signalingAddress, roomCode);
			if (!rendezvous->IsUsable())
			{
				Fail(rendezvous->FailureReason());
				return false;
			}
			channel = std::move(rendezvous);
		}
		m_ownedSignaling = std::move(channel);
		m_signaling = m_ownedSignaling.get();
		return true;
	}

	bool NetTraversalSession::HostWithCode(std::string_view roomCode, std::uint16_t port, int maxConnections)
	{
		ResetForNewAttempt();

		const auto normalized = NormalizeRoomCode(roomCode);
		if (!normalized.has_value())
		{
			Fail("'" + std::string(roomCode) + "' is not a valid room code");
			return false;
		}
		if (!EnsureSignalingChannel(*normalized))
		{
			return false;
		}

		m_isHostRole = true;
		if (!m_transport.Host(port, maxConnections))
		{
			Fail(m_transport.LastError());
			return false;
		}
		m_localPort = port;

		// The FIRST rung: ask the router, which needs no signalling channel and no
		// peer at all. Request() returning false means the library itself could not
		// start - not worth waiting on, so the ladder falls straight to the punch.
		if (m_portMapping.Request(port))
		{
			m_state = TraversalState::Mapping;
		}
		else
		{
			BeginSignalingAndPunch();
		}
		return true;
	}

	bool NetTraversalSession::JoinByCode(std::string_view roomCode)
	{
		ResetForNewAttempt();

		const auto normalized = NormalizeRoomCode(roomCode);
		if (!normalized.has_value())
		{
			Fail("'" + std::string(roomCode) + "' is not a valid room code");
			return false;
		}
		if (!EnsureSignalingChannel(*normalized))
		{
			return false;
		}

		m_isHostRole = false;
		bool bound = false;
		std::uint16_t boundPort = 0;
		for (int i = 0; i < kJoinerPortAttempts; ++i)
		{
			boundPort = static_cast<std::uint16_t>(kJoinerPortRangeStart + i);
			if (m_transport.Host(boundPort, 1))
			{
				bound = true;
				break;
			}
		}
		if (!bound)
		{
			Fail("could not bind a local port to join from - " + m_transport.LastError());
			return false;
		}
		m_localPort = boundPort;

		// A joiner never needs PortMapping - nothing is ever dialled INTO a joiner,
		// only out of it - so the ladder starts at the second rung directly.
		m_joinInProgress = true;
		BeginSignalingAndPunch();
		return true;
	}

	void NetTraversalSession::BeginSignalingAndPunch()
	{
		m_portMapping.Release();
		m_rendezvous.emplace(*m_transport.Traversal(), *m_signaling, m_localPort);
		m_rendezvous->Begin(m_stunHost, m_stunPort);
		m_state = TraversalState::Signaling;
	}

	void NetTraversalSession::TickMapping(float deltaSeconds)
	{
		m_portMapping.Tick();
		switch (m_portMapping.GetState())
		{
		case PortMapping::State::Mapped:
		{
			// A mapping means no punch is needed at all (PortMapping.hpp:73-80) - and
			// tearing a WORKING mapping down to punch anyway would only make a host
			// that is already reachable look like one that might not be. The mapped
			// address still has to reach the peer somehow, so it is published as a
			// candidate exactly like a STUN answer would be, then this is done.
			CandidateSet candidates;
			candidates.endpoints = NatTraversal::LocalCandidates(m_localPort);
			if (const auto mapped = NatTraversal::ParseEndpoint(m_portMapping.ExternalHost(), m_portMapping.ExternalPort()))
			{
				candidates.endpoints.push_back(*mapped);
			}
			m_signaling->Publish(candidates);
			AE_INFO(LogCategory::App, "Net: reachable via a router mapping - no punch needed");
			m_state = TraversalState::Connected;
			break;
		}
		case PortMapping::State::Unavailable:
			BeginSignalingAndPunch();
			break;
		case PortMapping::State::Requesting:
			m_mappingElapsed += deltaSeconds;
			if (m_mappingElapsed >= kMappingTimeoutSeconds)
			{
				BeginSignalingAndPunch();
			}
			break;
		case PortMapping::State::Idle:
			// Request() always moves past Idle before this is ever ticked; treated
			// the same as an outright refusal if it somehow lands here anyway.
			BeginSignalingAndPunch();
			break;
		}
	}

	void NetTraversalSession::TickRendezvous(float deltaSeconds)
	{
		m_rendezvous->Tick(deltaSeconds);
		const NatRendezvous::State state = m_rendezvous->GetState();
		if (state == NatRendezvous::State::Failed)
		{
			// Symmetric NAT or a peer who never joined - NatTraversal and NatRendezvous
			// already say which, plainly, in their own FailureReason (see
			// NatTraversal.hpp:27-30). Done with the punch either way: NatRendezvous
			// itself never retries past Failed (see its own class comment), but the
			// NatTraversal it wraps - and the socket m_transport shares with it - is
			// untouched by resetting just this wrapper, which is exactly what a relay
			// attempt needs: the same socket, one rung further down the ladder.
			const std::string punchFailure = m_rendezvous->FailureReason();
			m_rendezvous.reset();

			if (m_allowRelay && !m_turnHost.empty())
			{
				NatTraversal* traversal = m_transport.Traversal();
				if (traversal != nullptr && traversal->BeginRelay(m_turnHost, m_turnPort, m_turnUsername, m_turnPassword))
				{
					AE_INFO(LogCategory::App, "Net: the punch failed - trying the configured relay before giving up");
					m_relayPublished = false;
					m_relayElapsed = 0.0f;
					m_state = TraversalState::Relaying;
					return;
				}
				// BeginRelay refused outright - turnHost itself would not resolve. Named
				// specifically, because this is a DIFFERENT problem from the punch
				// failing (and a different fix: check turnHost/turnPort, not the punch's
				// own NAT-shaped advice) - see NatTraversal::BeginRelay.
				Fail("the punch failed and the configured relay '" + m_turnHost + "' could not be reached - "
				        + (traversal != nullptr ? traversal->RelayFailureReason() : std::string("no socket left to relay through")));
				return;
			}
			Fail(punchFailure);
			return;
		}
		if (state == NatRendezvous::State::Open)
		{
			const auto path = m_rendezvous->OpenPath();
			if (!path.has_value())
			{
				Fail("the path reported open named no endpoint");
				return;
			}
			// Only the JOINER dials: ConnectThrough is what turns the punched hole
			// into a real ENet handshake, and NetworkSubsystem.hpp:37-49 is explicit
			// that this must never be Connect(), which would rebind and discard the
			// hole the punch just opened. The host does nothing extra here - it is
			// already listening, and the joiner's CONNECT arrives at it exactly like
			// any other inbound connection.
			if (!m_isHostRole && !m_transport.ConnectThrough(*path))
			{
				Fail(m_transport.LastError());
				return;
			}
			m_connectingElapsed = 0.0f;
			m_state = TraversalState::Connecting;
			return;
		}
		m_state = (state == NatRendezvous::State::Punching) ? TraversalState::Punching : TraversalState::Signaling;
	}

	void NetTraversalSession::TickRelay(float deltaSeconds)
	{
		NatTraversal* traversal = m_transport.Traversal();
		if (traversal == nullptr)
		{
			Fail("the socket closed before the relay could reach the peer");
			return;
		}
		traversal->Tick(deltaSeconds);

		// Watched first, on every tick, for both roles: only the joiner ever calls
		// ConnectThrough below, but the host is just as capable of being the one this
		// arrives for - the joiner's CONNECT lands on it exactly like any other inbound
		// packet once NatTraversal has unwrapped it off the relay.
		for (const NetEvent& event: m_transport.Events())
		{
			if (event.kind == NetEvent::Kind::Connected)
			{
				AE_INFO(LogCategory::App, "Net: connected through the relay");
				m_state = TraversalState::Connected;
				m_joinInProgress = false;
				return;
			}
			if (event.kind == NetEvent::Kind::Disconnected)
			{
				Fail("the connection closed before it finished - the relayed path may have gone stale");
				return;
			}
		}

		if (traversal->RelayState() == TurnClient::State::Failed)
		{
			// Distinct from every punch-failure reason above: a relay that never
			// answers, or refuses the configured credentials, is a problem with the
			// relay - naming it is what lets a player fix THAT rather than re-check a
			// router setting that was never the issue here.
			Fail("the relay '" + m_turnHost + "' did not answer - " + traversal->RelayFailureReason());
			return;
		}

		if (!m_relayPublished && traversal->RelayState() == TurnClient::State::Allocated)
		{
			// Permitting every candidate the peer already offered - exactly what the
			// punch was tried against, see NatTraversal::PeerCandidates - is what lets
			// ITS packets through this new address at all; without a permission the
			// relay drops them, punch or no punch.
			for (const NatTraversal::Endpoint& peer: traversal->PeerCandidates())
			{
				traversal->PermitRelayPeer(peer);
			}
			// The relayed transport address is just another candidate - see
			// NatTraversal.hpp - so it goes out over the exact same channel the LAN and
			// reflexive candidates already did.
			CandidateSet candidates;
			candidates.endpoints = NatTraversal::LocalCandidates(m_localPort);
			if (const auto relayed = traversal->RelayedEndpoint())
			{
				candidates.endpoints.push_back(*relayed);
			}
			m_signaling->Publish(candidates);
			m_relayPublished = true;

			// Only the JOINER dials (see the class comment / TickRendezvous) - and
			// when it is THIS session's own relay that just came up, RelayConnectEndpoint()
			// is what it must dial: ENet's own sends have to land on the loopback
			// bridge, never on RelayedEndpoint() itself, which is the address just
			// published above for the PEER to use instead (see NatTraversal.hpp and
			// TurnRelaySocket.hpp for why confusing the two breaks the relay). The
			// host does nothing extra here, exactly like every other rung - it stays
			// passively listening, and the first datagram this relay forwards already
			// arrives sourced from that same loopback address, which auto-creates the
			// peer ENet needs with no explicit dial at all.
			if (!m_isHostRole)
			{
				if (const auto connectEndpoint = traversal->RelayConnectEndpoint())
				{
					if (m_transport.ConnectThrough(*connectEndpoint))
					{
						m_connectingElapsed = 0.0f;
						m_state = TraversalState::Connecting;
						return;
					}
				}
			}
		}

		// A joiner is the only role that ever dials (see the class comment) - so once
		// the peer has had its own chance to relay, its candidate arrives here exactly
		// like any other, just later, over the same channel. Only a candidate that was
		// NOT already tried during the punch is worth a fresh ConnectThrough: anything
		// already in PeerCandidates() is one this socket already failed to reach
		// directly, and dialling it again would only repeat that failure.
		if (!m_isHostRole)
		{
			if (const auto offered = m_signaling->Poll())
			{
				const std::vector<NatTraversal::Endpoint> alreadyTried = traversal->PeerCandidates();
				for (const NatTraversal::Endpoint& candidate: offered->endpoints)
				{
					if (std::ranges::find(alreadyTried, candidate) != alreadyTried.end())
					{
						continue;
					}
					if (m_transport.ConnectThrough(candidate))
					{
						m_connectingElapsed = 0.0f;
						m_state = TraversalState::Connecting;
						return;
					}
				}
			}
		}

		m_relayElapsed += deltaSeconds;
		if (m_relayElapsed >= kRelayTimeoutSeconds)
		{
			Fail("the relay never produced a path to the peer");
		}
	}

	void NetTraversalSession::TickConnecting(float deltaSeconds)
	{
		for (const NetEvent& event: m_transport.Events())
		{
			if (event.kind == NetEvent::Kind::Connected)
			{
				AE_INFO(LogCategory::App, "Net: connected over a punched path");
				m_state = TraversalState::Connected;
				m_joinInProgress = false;
				return;
			}
			if (event.kind == NetEvent::Kind::Disconnected)
			{
				Fail("the connection closed before it finished - the punched path may have gone stale");
				return;
			}
		}
		m_connectingElapsed += deltaSeconds;
		if (m_connectingElapsed >= kConnectingTimeoutSeconds)
		{
			Fail("timed out completing the connection after the path opened");
		}
	}

	void NetTraversalSession::Tick(float deltaSeconds)
	{
		switch (m_state)
		{
		case TraversalState::Idle:
		case TraversalState::Connected:
		case TraversalState::Failed:
			break; // nothing left to drive
		case TraversalState::Mapping:
			TickMapping(deltaSeconds);
			break;
		case TraversalState::Signaling:
		case TraversalState::Punching:
			TickRendezvous(deltaSeconds);
			break;
		case TraversalState::Relaying:
			TickRelay(deltaSeconds);
			break;
		case TraversalState::Connecting:
			TickConnecting(deltaSeconds);
			break;
		}
	}

	void NetTraversalSession::Fail(std::string reason)
	{
		m_failure = std::move(reason);
		m_state = TraversalState::Failed;
		AE_WARN(LogCategory::App, "Net: could not reach the peer - {}", m_failure);

		// A failed attempt must not leave the socket half set up: it would go on
		// claiming NetRole::Host for a joiner that connected to nobody (see
		// JoinInProgress), and a caller would otherwise have to remember to close it
		// by hand before HostWithCode/JoinByCode can be tried again.
		m_rendezvous.reset();
		m_portMapping.Release();
		m_transport.Disconnect();
		m_joinInProgress = false;
	}
} // namespace aether::net
