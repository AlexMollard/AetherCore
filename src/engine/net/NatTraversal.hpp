#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/StunMessage.hpp"

struct _ENetHost;
struct _ENetEvent;

namespace aether::net
{
	// Opens a path to a peer without anyone forwarding a port, by discovering what the
	// outside world sees this socket as and then punching toward the peer's equivalent.
	//
	// It works on the socket ENet already owns, and that is the whole design. A NAT
	// mapping belongs to the socket that created it: punch from a second socket, or from
	// a library that insists on binding its own, and the hole is open for a source port
	// ENet will never send from. So the STUN and connectivity-check traffic goes out
	// through `host->socket`, and comes back in through `host->intercept`, which exists
	// precisely so a caller can take datagrams off the wire before ENet reads them as its
	// own protocol.
	//
	// What this deliberately does NOT do is relay. A symmetric NAT gives every
	// destination a different mapping, so no punch can succeed, and the only fix is a
	// server in the middle. Peers behind one end at Failed with that said plainly rather
	// than retrying forever - see the state below.
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
	};
} // namespace aether::net
