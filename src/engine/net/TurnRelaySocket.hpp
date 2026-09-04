#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <enet/enet.h>

#include "net/TurnClient.hpp"

namespace aether::net
{
	// Bridges an ENet host to a TURN relay allocation, without an ENet send-side hook -
	// ENet has none. `host->intercept` (build/default/_deps/enet-src/include/enet/enet.h:346-397)
	// only ever sees RECEIVED datagrams; nothing calls back before ENet sends its OWN
	// traffic. So once ENet learns a peer's address, everything it sends there itself -
	// acks, retransmits, reliable data - goes straight out via enet_socket_send to that
	// exact address, bypassing any relay entirely. A relayed peer's real address is
	// exactly what a symmetric NAT refuses, which is the entire reason a relay exists,
	// so that breaks relaying in the outbound direction no matter how correct the
	// inbound unwrap is.
	//
	// The fix needs no ENet hook at all: present the relayed peer to ENet as
	// 127.0.0.1:<ProxyPort()> instead of its real address (see
	// NatTraversal::RelayConnectEndpoint - NEVER NatTraversal::RelayedEndpoint, which is
	// the address published to the peer over signalling and must never reach ENet
	// itself). ENet's OWN socket then sends every reply there - a plain, ordinary send
	// to a plain, ordinary loopback address, no hook required - and this class owns the
	// other end of that bridge:
	//
	//   ENet host                  this class                       TURN server
	//   127.0.0.1:enetPort  <-->  loopback socket (127.0.0.1:ProxyPort())
	//                             <-> TurnClient <-> relay socket  <-->  turnHost:turnPort
	//
	// Two sockets, not one: a socket BOUND to 127.0.0.1 cannot itself reach a real
	// address on the internet - the kernel has nowhere to route a packet whose source
	// address is loopback once the destination is not (and the constraint that this
	// class's loopback socket bind to 127.0.0.1, never a wildcard address, is what makes
	// it unreachable from the network in the first place - see kLoopbackAddress in the
	// .cpp). So the loopback socket talks only to ENet, and a second, ordinarily-bound
	// socket (ANY-bound, ephemeral port) is what TurnClient's own SendFn actually sends
	// the relay's own traffic through.
	//
	// This deliberately does NOT reuse the socket ENet's own host owns for the relay
	// leg, unlike everything else in this subsystem - STUN discovery, the punch, and
	// (before this class existed) the TURN control channel too; see NatTraversal.hpp's
	// class comment for why THAT socket-sharing rule exists. That rule is about hole
	// punching: a NAT mapping belongs to the socket that created it, because punching
	// from a second socket opens a hole for a port nothing ever sends from. A TURN
	// relay needs no hole at all - a permitted peer sends straight to the relay's own
	// public address, which nothing here ever has to punch toward - so giving the relay
	// leg its own socket pair here is not a violation of that rule; there is no hole to
	// lose.
	//
	// One instance identifies exactly one relayed peer, because a single loopback
	// address is all ENet has to tell peers apart by. PermitPeer may be called more than
	// once - exactly like the punch trying a peer's public AND its LAN candidate - but
	// every address passed is treated as another way to reach the SAME peer this proxy
	// bridges, never a second, different one: outbound traffic is wrapped toward every
	// permitted candidate (harmless for whichever ones are not actually reachable - a
	// send to a bogus address just vanishes, same as ordinary UDP loss), and inbound
	// traffic from any of them is delivered to ENet under the one loopback identity this
	// instance owns. A session that ever needs several DIFFERENT relayed peers connected
	// through the same host at once - plausible for this engine's small co-op sessions,
	// but nothing today ever asks for it - needs one NatTraversal::BeginRelay call (and
	// therefore one TurnRelaySocket, one allocation, one proxy port) per peer, not a
	// change to this class.
	class TurnRelaySocket
	{
	public:
		// `enetLoopbackPort` is the ENet host's own bound port (read from
		// enet_socket_get_address(host->socket, ...), NOT trusted from ENetHost::address -
		// see NatTraversal::BeginRelay for why): inbound relay payloads are forwarded to
		// 127.0.0.1:<that port> so ENet reads them as an ordinary datagram, sourced from
		// this proxy's own port, exactly as if the relayed peer had sent them directly.
		// `server`/`username`/`password` configure the underlying TurnClient exactly like
		// NatTraversal::BeginRelay's own parameters, unchanged since before this class
		// existed.
		TurnRelaySocket(std::uint16_t enetLoopbackPort, TurnClient::Endpoint server, std::string username, std::string password);
		~TurnRelaySocket();

		TurnRelaySocket(const TurnRelaySocket&) = delete;
		TurnRelaySocket& operator=(const TurnRelaySocket&) = delete;

		// False if either local socket could not be created or bound - both are
		// ephemeral/loopback binds on this machine, so this should not realistically
		// happen, but a caller that gets false has nothing to poll and must not proceed
		// (NatTraversal::BeginRelay treats it exactly like a resolve failure).
		[[nodiscard]] bool IsValid() const
		{
			return m_loopbackSocket != ENET_SOCKET_NULL && m_relaySocket != ENET_SOCKET_NULL;
		}

		// The loopback port ENet's own outbound sends must be aimed at - see
		// NatTraversal::RelayConnectEndpoint, the only caller that needs this number.
		[[nodiscard]] std::uint16_t ProxyPort() const
		{
			return m_proxyPort;
		}

		// Starts (or restarts) the Allocate handshake - see TurnClient::BeginAllocate.
		void BeginAllocate();
		// Best-effort teardown - see TurnClient::Release. Goes out over THIS class's own
		// relay-facing socket, independent of ENet's host entirely.
		void Release();

		// Ticks the underlying TurnClient state machine, then drains both sockets in
		// each direction. Call once per frame with real seconds, exactly like every
		// other Tick in this subsystem - nothing here runs on a thread, so a delayed
		// Tick just looks like extra latency, never a race with anything else that
		// touches these sockets.
		void Tick(float deltaSeconds);

		[[nodiscard]] TurnClient::State RelayState() const
		{
			return m_turn.GetState();
		}

		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_turn.FailureReason();
		}

		// The REAL relayed transport address - what gets published to the peer over
		// signalling. NEVER what ENet is told the peer's address is; see ProxyPort()
		// above for that one.
		[[nodiscard]] std::optional<TurnClient::Endpoint> RelayedEndpoint() const
		{
			return m_turn.RelayedEndpoint();
		}

		// Installs (or refreshes) a relay permission for `peer` and remembers it as one
		// of the addresses outbound ENet traffic should be wrapped toward - see the
		// class comment above for why more than one call here still names one peer.
		void PermitPeer(const TurnClient::Endpoint& peer);

	private:
		// ENet -> relay: drains every datagram ENet's own socket sent to the loopback
		// bridge this Tick, wrapping each and handing it to every currently-permitted
		// peer candidate. Validates size and source before touching anything a
		// datagram's contents claim - see the .cpp for exactly what is checked and why.
		void PumpEnetToRelay();
		// relay -> ENet: drains every datagram waiting on the relay-facing socket,
		// unwraps whatever TurnClient recognises as a peer's payload, and hands it to
		// ENet's own bound loopback port, sourced from this proxy's own port so ENet
		// sees a consistent peer identity in both directions.
		void PumpRelayToEnet();

		void SendToRelay(const TurnClient::Endpoint& to, std::span<const std::uint8_t> bytes);
		void ForwardToEnet(std::span<const std::byte> payload);

		std::uint16_t m_enetLoopbackPort = 0;
		ENetSocket m_loopbackSocket = ENET_SOCKET_NULL;
		ENetSocket m_relaySocket = ENET_SOCKET_NULL;
		std::uint16_t m_proxyPort = 0;
		TurnClient m_turn;

		// Candidate addresses for the ONE peer this proxy bridges - see the class
		// comment for why this is never more than one peer, however many candidates it
		// holds.
		std::vector<TurnClient::Endpoint> m_peers;
	};
} // namespace aether::net
