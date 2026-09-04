#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/StunMessage.hpp"
#include "net/TurnClient.hpp"

struct _ENetHost;
struct _ENetEvent;

namespace aether::net
{
	class TurnRelaySocket;

	// Opens a path to a peer without anyone forwarding a port, by discovering what the
	// outside world sees this socket as and then punching toward the peer's equivalent.
	//
	// It works on the socket ENet already owns, and that is the whole design - for
	// BeginDiscovery and BeginPunch. A NAT mapping belongs to the socket that created
	// it: punch from a second socket, or from a library that insists on binding its
	// own, and the hole is open for a source port ENet will never send from. So the
	// STUN and connectivity-check traffic goes out through `host->socket`, and comes
	// back in through `host->intercept`, which exists precisely so a caller can take
	// datagrams off the wire before ENet reads them as its own protocol.
	//
	// BeginRelay below is different, on purpose: a TURN relay needs no hole at all - a
	// permitted peer sends straight to the relay's own public address, which nothing
	// here ever has to punch toward - so its traffic does NOT travel over `host->socket`
	// at all. It travels over a TurnRelaySocket, a small proxy that owns its own
	// dedicated socket pair and presents the relayed peer to ENet as an ordinary
	// loopback address (see TurnRelaySocket.hpp for the full picture, including the
	// send-side problem `host->intercept` alone cannot solve: ENet has no symmetric
	// hook for OUTBOUND datagrams, so a relayed peer whose real address ENet ever
	// learned directly would have every reply sent straight past the relay to that
	// address - exactly what a symmetric NAT refuses).
	//
	// What this deliberately does NOT do is relay ON ITS OWN, in the sense of guessing a
	// server to use. A symmetric NAT gives every destination a different mapping, so no
	// punch can succeed, and the only fix is a server in the middle. Nothing here picks
	// a relay to trust; that is a caller's (NetTraversalSession's) decision, gated on
	// `network.allowRelay` being turned on and a `network.turnHost` being configured.
	//
	// IPv4 only, because ENetAddress is: a 32-bit host and a 16-bit port with nowhere to
	// put anything longer.
	class NatTraversal
	{
	public:
		// Endpoints are ENet's convention, which is not uniform and is easy to get
		// backwards: `host` is in NETWORK byte order and `port` is in HOST order. The
		// conversion from STUN's all-host-order form happens here, at the boundary,
		// rather than being an assumption spread across callers.
		struct Endpoint
		{
			std::uint32_t host = 0;
			std::uint16_t port = 0;

			friend bool operator==(const Endpoint&, const Endpoint&) = default;
		};

		enum class State
		{
			Idle,        // nothing asked for yet
			Discovering, // waiting for a STUN server to answer
			Discovered,  // public endpoint known, ready to be signalled to the peer
			Punching,    // sending connectivity checks at the peer's candidates
			Open,        // a check was answered: that path carries traffic
			Failed,      // nothing answered in time; needs forwarding or a relay
		};

		explicit NatTraversal(_ENetHost* host);
		~NatTraversal();

		NatTraversal(const NatTraversal&) = delete;
		NatTraversal& operator=(const NatTraversal&) = delete;

		// Ask a STUN server for this socket's public endpoint. Returns false only if the
		// server name cannot be resolved; the exchange itself is asynchronous, so poll
		// GetState() afterwards.
		bool BeginDiscovery(const std::string& stunHost, std::uint16_t stunPort = 3478);

		// What the outside world sees, once Discovered. This is the value to hand the
		// peer through whatever signalling channel is in use.
		[[nodiscard]] std::optional<Endpoint> PublicEndpoint() const
		{
			return m_public;
		}

		// Start connectivity checks toward everything the peer offered. Both its public
		// endpoint and its address on its own LAN are worth trying: two players in one
		// house reach each other directly, and no NAT is involved at all.
		void BeginPunch(std::span<const Endpoint> peerCandidates);

		// The endpoint that answered, once Open. Connect to it with
		// NetworkSubsystem::ConnectThrough, NOT by binding a new socket: the mapping that
		// makes it reachable belongs to THIS socket, and a connect that rebinds discards
		// the hole this whole exchange existed to open.
		[[nodiscard]] std::optional<Endpoint> OpenPath() const
		{
			return m_open;
		}

		[[nodiscard]] State GetState() const
		{
			return m_state;
		}

		// Why it failed, for a player who now has to be told something useful.
		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_failure;
		}

		// Drives retransmission and timeouts. UDP loses datagrams and a punch is a race,
		// so a single send proves nothing; call this once per frame with real seconds.
		void Tick(float deltaSeconds);

		// The addresses this socket has on its own network interfaces, as candidates to
		// offer alongside the public one.
		[[nodiscard]] static std::vector<Endpoint> LocalCandidates(std::uint16_t port);

		// Starts a TURN allocation (RFC 5766), fronted by a TurnRelaySocket - the
		// fallback for exactly the case a punch cannot solve, tried only once
		// BeginPunch has already given up (see NetTraversalSession, which owns that
		// ordering). Unlike BeginDiscovery/BeginPunch above, this does NOT reuse
		// `host->socket`: a relay needs no hole punched toward it (see
		// TurnRelaySocket.hpp's class comment for why that is not a violation of this
		// class's own socket-sharing rule), so the proxy opens its own dedicated
		// socket pair instead. The loopback half of that pair is bridged to THIS
		// socket's real bound port - read via enet_socket_get_address(host->socket,
		// ...), never trusted from ENetHost::address, which is not reliable here. Safe
		// to call even while m_state is Failed - relaying is independent bookkeeping
		// from the punch, not a state it transitions through. Returns false if
		// `turnHost` cannot be resolved, or if the proxy's own local sockets could not
		// be opened (TurnRelaySocket::IsValid() false, treated exactly like a resolve
		// failure - see its own comment); the allocation itself is asynchronous either
		// way, exactly like BeginDiscovery - poll RelayState()/RelayedEndpoint()/
		// RelayConnectEndpoint()/RelayFailureReason() afterwards.
		bool BeginRelay(const std::string& turnHost, std::uint16_t turnPort, std::string username, std::string password);

		// Out of line: TurnRelaySocket is only forward-declared here (see the class
		// comment), so a member access through m_relay needs the full definition this
		// header deliberately does not include.
		[[nodiscard]] TurnClient::State RelayState() const;

		// XOR-RELAYED-ADDRESS from the allocation, once RelayState() is Allocated: the
		// PUBLIC address this relay makes reachable. PUBLISH THIS to the peer over
		// signalling, exactly like PublicEndpoint() above - see NetTraversalSession's
		// Relaying rung, the only caller that needs to know a relay is involved at
		// all. NEVER hand this to ENet - see RelayConnectEndpoint() below for the one
		// that goes there instead.
		[[nodiscard]] std::optional<Endpoint> RelayedEndpoint() const;

		// `127.0.0.1:<the proxy's port>` - what ENet must be told the relayed peer's
		// address is, via NetworkSubsystem::ConnectThrough. NEVER publish this to the
		// peer over signalling - it names nothing outside this machine; see
		// RelayedEndpoint() above for the address that actually does. std::nullopt
		// exactly when RelayedEndpoint() is: no allocation, no address either kind.
		[[nodiscard]] std::optional<Endpoint> RelayConnectEndpoint() const;

		// Why RelayState() is Failed, for a player who has to be told something useful -
		// distinct from FailureReason() above, which is the PUNCH's own reason and is
		// normally already stale by the time a relay is even tried.
		[[nodiscard]] const std::string& RelayFailureReason() const;

		// Installs a relay permission for `peer` - see PeerCandidates() below for why the
		// caller is expected to pass candidates from there rather than a guess. A no-op
		// unless a relay allocation is up.
		void PermitRelayPeer(const Endpoint& peer);

		// The candidates BeginPunch was last called with - what the peer offered over
		// signalling that a punch was tried against. NetTraversalSession's Relaying rung
		// reuses this to decide who to PermitRelayPeer, rather than keeping (and risking
		// drifting from) a second copy of a list that already lives here.
		[[nodiscard]] std::vector<Endpoint> PeerCandidates() const;

		// A candidate written as text, which is how one arrives from a peer over any
		// signalling channel. Refuses a name that needs a DNS lookup: a candidate is an
		// address a NAT has already reported, and resolving something else here would
		// aim a punch at a host nobody negotiated with.
		[[nodiscard]] static std::optional<Endpoint> ParseEndpoint(const std::string& address, std::uint16_t port);

		// The dotted-quad form, for putting a candidate into a signalling payload.
		[[nodiscard]] static std::string FormatAddress(const Endpoint& endpoint);

	private:
		// Called from ENet's intercept while polling. Returns true when the datagram was
		// ours, which stops ENet from trying to parse it.
		bool OnDatagram(const Endpoint& from, std::span<const std::byte> data);
		static int InterceptThunk(_ENetHost* host, _ENetEvent* event);

		void Send(const Endpoint& to, std::span<const std::uint8_t> bytes);
		void Fail(std::string reason);

		_ENetHost* m_host = nullptr;

		State m_state = State::Idle;
		std::string m_failure;

		Endpoint m_stunServer{};
		stun::TransactionId m_discoveryId{};
		std::optional<Endpoint> m_public;

		struct Check
		{
			Endpoint target{};
			stun::TransactionId id{};
		};
		std::vector<Check> m_checks;
		std::optional<Endpoint> m_open;

		float m_elapsed = 0.0f;
		float m_sinceRetry = 0.0f;
		int m_attempts = 0;

		// Independent of m_state above: a relay is tried only after a punch has already
		// given up, but nothing here requires m_state to BE Failed, so the two are kept
		// as separate bookkeeping rather than folded into one enum a caller would have to
		// unpick.
		std::unique_ptr<TurnRelaySocket> m_relay;
		// Set only when BeginRelay itself could not resolve turnHost - RelayFailureReason()
		// falls back to this exactly when m_relay was never even constructed.
		std::string m_relayResolveFailure;
	};
} // namespace aether::net
