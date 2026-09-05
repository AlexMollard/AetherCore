#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "net/NatRendezvous.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/PortMapping.hpp"
#include "net/Signaling.hpp"

namespace aether::net
{
	// Where a punch attempt currently stands, in the order the ladder tries them. A
	// UI renders this directly - see NetworkContext::GetTraversalState - so these
	// names are the whole vocabulary a status line has to work with.
	enum class TraversalState : std::uint8_t
	{
		Idle,       // nothing asked for yet
		Mapping,    // asking the router for a port (host role only)
		Signaling,  // publishing candidates over the room and waiting for the peer's
		Punching,   // sending connectivity checks at whatever the peer offered
		Relaying,   // the punch failed; asking a configured TURN relay for a path instead
		Connecting, // a path opened; waiting for ENet's own handshake over it
		Connected,  // done - NetworkReceiveSystem/NetworkSendSystem own the link now
		Failed,     // no path reached the peer; see FailureReason
	};

	// Which room-code service a session's candidates travel over. Both backends sit
	// behind ISignalingChannel - the seam a future hosted backend (a lobby SDK) drops
	// into - so this class never names BroadcastSignalingChannel or RendezvousChannel
	// outside the two branches that construct one.
	enum class SignalingBackend : std::uint8_t
	{
		LanBroadcast,
		Rendezvous,
	};

	// Drives one connect attempt through the whole ladder: ask the router, and if
	// that does not make this socket reachable, publish candidates over a room code,
	// punch at whatever the peer offers, and finish the real ENet handshake through
	// the hole that opens. See the comment atop NatTraversal.hpp for why every step
	// of this runs over the SAME socket the transport owns rather than one of its
	// own - PortMapping and the TurnRelaySocket a relay allocation fronts are the two
	// exceptions, and neither touches ENet's own data socket: PortMapping talks to the
	// router, not the peer, and a TurnRelaySocket needs no hole punched toward it at
	// all (see TurnRelaySocket.hpp's own class comment for why that is not a
	// violation of this same rule).
	//
	// BOTH roles call NetworkSubsystem::Host() first (NetworkSubsystem.hpp:37-49) - a
	// host to be reachable at all, a joiner because the punch and the ConnectThrough
	// that follows it must run over the exact socket that punched. That makes a
	// JOINER's role transiently misleading: NetworkSubsystem reports NetRole::Host
	// the instant that socket is bound, for a peer that is not hosting anything and
	// has connected to nobody. JoinInProgress() is what lets a caller (NetworkContext)
	// correct for that without this class knowing NetworkContext exists.
	class NetTraversalSession
	{
	public:
		explicit NetTraversalSession(NetworkSubsystem& transport);

		NetTraversalSession(const NetTraversalSession&) = delete;
		NetTraversalSession& operator=(const NetTraversalSession&) = delete;

		// Remembered for the HostWithCode/JoinByCode call that follows: neither
		// backend can be constructed until the room code is known (both bake it into
		// their wire encoding), so this only records the choice. Safe to call again
		// before a later attempt to switch backends.
		void ConfigureSignaling(SignalingBackend backend, std::string address);

		// Advanced/test seam: use this exact channel instead of constructing one from
		// ConfigureSignaling, and never reconstruct it on a later attempt. This is
		// what makes the whole ladder testable with no network - see
		// NetTraversalSessionTests.cpp pairing two sessions over a
		// LocalSignalingChannel - and, later, what a hosted backend (a lobby SDK that
		// builds its own channel) plugs into without this class knowing it exists.
		void ConfigureSignalingChannel(ISignalingChannel& channel);

		// Seeds the rendezvous backend from configuration (EngineSettings::Network::
		// rendezvousHost) WITHOUT overriding a game that already chose a backend
		// itself. An empty host leaves the default alone, which is LAN broadcast -
		// the one rung that needs no server anywhere.
		void SetRendezvousDefault(std::string host, std::uint16_t port);

		// Which backend the next attempt will actually use, and the address it will
		// use it with. Exposed because "how are we even trying to reach them" is
		// something a menu legitimately narrates - and because the precedence
		// SetRendezvousDefault implements is otherwise unobservable, so it could
		// regress silently.
		[[nodiscard]] SignalingBackend Backend() const
		{
			return m_backend;
		}

		[[nodiscard]] const std::string& SignalingAddress() const
		{
			return m_signalingAddress;
		}

		// Overrides the STUN server discovery asks for this socket's public endpoint.
		// Defaults to a well-known public one (see the .cpp) so ConfigureSignaling
		// alone is enough to punch across the internet. An empty host disables
		// discovery entirely - the LAN-only mode NatRendezvous::Begin already
		// supports for two players with nothing but a local network between them,
		// and what a test with no network reachability calls this with.
		void SetStunServer(std::string host, std::uint16_t port = 3478);

		// Overrides the TURN relay the ladder falls back to once a punch has genuinely
		// failed - see the Relaying state above. `allowRelay` mirrors
		// EngineSettings::Network::allowRelay: even a fully configured relay is never
		// tried unless this is true, because every packet through it costs someone
		// bandwidth (see NatTraversal.hpp). An empty `host` (the default, matching
		// EngineSettings::Network::turnHost's empty default) disables relaying entirely,
		// same shape as SetStunServer's empty-host case above - and what a test with no
		// relay reachable calls this with, or simply never calls at all.
		void SetTurnServer(std::string host, std::uint16_t port, std::string username, std::string password, bool allowRelay);

		// Returns false only when the attempt could not even begin (a malformed room
		// code, the local socket failing to bind) - GetState() is Failed in every
		// case this returns false, with FailureReason() saying why. A true return
		// means the ladder is running; watch GetState() from here.
		[[nodiscard]] bool HostWithCode(std::string_view roomCode, std::uint16_t port, int maxConnections);
		[[nodiscard]] bool JoinByCode(std::string_view roomCode);

		// Drives every phase: PortMapping, the rendezvous/punch, and watching for the
		// ENet handshake that follows an opened path. Call once per frame with real
		// seconds, same as NatRendezvous underneath it - and, on the joiner side,
		// after NetworkSubsystem::Poll() has run this frame, since watching for the
		// handshake reads Poll()'s own event list.
		void Tick(float deltaSeconds);

		[[nodiscard]] TraversalState GetState() const
		{
			return m_state;
		}

		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_failure;
		}

		// True from the moment a JOIN attempt binds its socket until ConnectThrough
		// runs or the attempt ends - see the class comment above.
		[[nodiscard]] bool JoinInProgress() const
		{
			return m_joinInProgress;
		}

	private:
		void ResetForNewAttempt();
		[[nodiscard]] bool EnsureSignalingChannel(const std::string& roomCode);
		void BeginSignalingAndPunch(std::optional<NatTraversal::Endpoint> mappedEndpoint = std::nullopt);
		void TickMapping(float deltaSeconds);
		void TickRendezvous(float deltaSeconds);
		void TickRelay(float deltaSeconds);
		void TickConnecting(float deltaSeconds);
		void Fail(std::string reason);

		NetworkSubsystem& m_transport;

		PortMapping m_portMapping;
		float m_mappingElapsed = 0.0f;

		std::string m_stunHost = "stun.l.google.com";
		std::uint16_t m_stunPort = 19302;

		std::string m_turnHost;
		std::uint16_t m_turnPort = 3478;
		std::string m_turnUsername;
		std::string m_turnPassword;
		bool m_allowRelay = false;

		SignalingBackend m_backend = SignalingBackend::LanBroadcast;
		// Whether a caller stated a backend explicitly, which SetRendezvousDefault
		// must not overwrite. See its comment for the precedence order.
		bool m_signalingChosen = false;
		std::string m_signalingAddress;
		bool m_useInjectedChannel = false;
		std::unique_ptr<ISignalingChannel> m_ownedSignaling;
		ISignalingChannel* m_signaling = nullptr;

		bool m_isHostRole = false;
		bool m_joinInProgress = false;
		std::uint16_t m_localPort = 0;

		std::optional<NatRendezvous> m_rendezvous;
		float m_connectingElapsed = 0.0f;

		// Set once the current relay attempt has published its candidate, so TickRelay
		// does not re-publish (and re-permit every peer candidate) on every single tick
		// while waiting for the peer to answer.
		bool m_relayPublished = false;
		float m_relayElapsed = 0.0f;

		TraversalState m_state = TraversalState::Idle;
		std::string m_failure;
	};
} // namespace aether::net
