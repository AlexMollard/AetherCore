#include "net/NetTraversalSession.hpp"

#include <utility>

#include "net/BroadcastSignaling.hpp"
#include "net/RendezvousChannel.hpp"
#include "net/RoomCode.hpp"
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
			// Symmetric NAT or a peer who never joined - NatTraversal and
			// NatRendezvous already say which, plainly, in their own FailureReason
			// (see NatTraversal.hpp:27-30); this just carries it through unchanged.
			Fail(m_rendezvous->FailureReason());
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
