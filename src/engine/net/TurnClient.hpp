#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "net/StunAttributes.hpp"

namespace aether::net
{
	// RFC 5766 TURN relay client: obtains a relayed transport address on a TURN server and
	// moves game datagrams through it once a direct path or a NatTraversal punch cannot reach a
	// peer - the case a symmetric NAT makes unsolvable by construction (see NatTraversal.hpp).
	//
	// Followed RFC 5766 rather than RFC 8656 throughout: 8656 folds TURN together with ICE's TCP
	// relaying and adds IPv6/DTLS transports, none of which this client, or the IPv4-only,
	// UDP-only rest of this subsystem, needs. 8656's Allocate / Refresh / CreatePermission /
	// ChannelBind / Send-and-Data wire format is unchanged from 5766 - where the two differ, it
	// is 8656 folding in 5766's own errata, not a behavioural difference that matters here.
	//
	// Message-only, exactly like StunMessage.hpp/StunAttributes.hpp and for the same reason: a
	// relay allocation belongs to the socket that requested it, so every byte has to travel
	// through the socket ENet already owns rather than one this class binds itself. So this
	// takes a send callback instead of a socket, and its caller (NatTraversal, in wave 3) feeds
	// it inbound datagrams straight from ENet's intercept. That is also what makes the whole
	// state machine - Allocate's 401/438 dance, refresh, permissions, channels - testable with a
	// fake server and no network at all.
	//
	// Every attribute this class reads off the wire is read the same way StunAttributes.hpp
	// reads them: validated before use, never trusted past a failed MESSAGE-INTEGRITY check, and
	// a malformed or unexpected message is dropped rather than acted on. Nothing here is ever
	// gated on AE_ASSERT, which compiles out in Ship - wire input from an unauthenticated relay
	// is exactly the input that must still be checked in a shipped build.
	class TurnClient
	{
	public:
		using Endpoint = stun::Endpoint;

		enum class State
		{
			Idle,       // BeginAllocate has not been called yet
			Allocating, // Allocate in flight - the unauthenticated probe, or an authenticated retry
			Allocated,  // relayed endpoint known; RelayedEndpoint() is valid and traffic can flow
			Failed,     // gave up; FailureReason() says why
			Released,   // Release() was called; the allocation was told to end immediately
		};

		using SendFn = std::function<void(const Endpoint& to, std::span<const std::uint8_t> bytes)>;

		// `server` is the TURN server's transport address, already resolved, in STUN's
		// host-order convention - the same one XOR-address attributes use, and the same one
		// NatTraversal converts ENet addresses to/from at its own socket boundary. `send` is
		// called with `server` as the destination for every message this client ever emits; the
		// caller sends those bytes out through `host->socket`, unchanged.
		TurnClient(Endpoint server, std::string username, std::string password, SendFn send);

		// Starts (or restarts, from any state) the Allocate handshake: the deliberately
		// unauthenticated first attempt, which a real server answers with 401 Unauthorized
		// carrying the REALM and NONCE the authenticated retry needs. Returns immediately; poll
		// GetState() afterwards.
		void BeginAllocate();

		// Best-effort teardown: while Allocated, sends one Refresh with LIFETIME 0 so the relay
		// drops the allocation immediately rather than waiting out its LIFETIME, then moves to
		// Released without waiting for a reply - by the time a caller releases, it is normally
		// shutting down, and the relay expires the allocation on its own even if this datagram
		// is lost. A no-op outside Allocating/Allocated.
		void Release();

		// Drives retransmission, allocation refresh, and permission/channel upkeep. Call once
		// per frame with real seconds, exactly like NatTraversal::Tick and PortMapping::Tick.
		void Tick(float deltaSeconds);

		// What OnDatagram found in one datagram.
		struct Delivery
		{
			// True when the datagram belonged to this client and must NOT be handed to ENet (or
			// anything else) as though it were unrelated traffic - every byte the relay ever
			// sends, control reply or relayed peer data, arrives from `server`, and nothing a
			// peer sends directly ever does, so this is decided purely by source address.
			bool consumed = false;

			// Set only when the datagram carried an application payload relayed from a peer (a
			// Data indication or a ChannelData message): `peer` is who it is from, `payload` is
			// what to hand ENet as though the datagram had arrived from `peer` directly.
			// `payload` aliases the buffer passed to OnDatagram and is valid only as long as
			// that buffer is - copy it before the caller's next poll if it needs to outlive one.
			std::optional<Endpoint> peer;
			std::span<const std::byte> payload;
		};

		// Feed this a datagram taken off the ENet-owned socket, with `from` naming who sent it.
		[[nodiscard]] Delivery OnDatagram(const Endpoint& from, std::span<const std::byte> data);

		[[nodiscard]] State GetState() const
		{
			return m_state;
		}

		// Why it is Failed, for a player who has to be told something useful. Distinguishes a
		// misconfigured relay (bad turnUsername/turnPassword) from an unreachable one, because a
		// player can only act on one of those.
		[[nodiscard]] const std::string& FailureReason() const
		{
			return m_failure;
		}

		// The TURN server this client talks to.
		[[nodiscard]] const Endpoint& Server() const
		{
			return m_server;
		}

		// XOR-RELAYED-ADDRESS from the Allocate success response, once Allocated: the candidate
		// this relay makes available to a peer, and what makes a relayed path just another ICE
		// candidate for wave 3 to publish rather than a special case.
		[[nodiscard]] std::optional<Endpoint> RelayedEndpoint() const
		{
			return m_relayed;
		}

		// Installs (or refreshes) permission for `peer`'s traffic to cross the relay, and starts
		// trying to bind it to a channel number so later SendToPeer traffic can move as 4-byte
		// ChannelData instead of a ~36-byte Send indication. Safe to call repeatedly - Tick()
		// keeps whatever gets installed alive on its own afterwards.
		void PermitPeer(const Endpoint& peer);

		// Sends `payload` to `peer` through the relay: as ChannelData once a channel is bound to
		// it, otherwise as a Send indication while the bind is still outstanding. Implicitly
		// calls PermitPeer(peer) first - nothing reaches a peer the relay has no permission
		// for. Requires Allocated; a call before then is dropped, matching ordinary UDP loss.
		void SendToPeer(const Endpoint& peer, std::span<const std::byte> payload);

	private:
		// One outstanding request, tracked so it can be retransmitted verbatim (RFC 5389 s7.2.1
		// requires the SAME transaction id and body on a retransmit, not a new transaction) and
		// eventually given up on.
		struct PendingExchange
		{
			bool active = false;
			stun::TransactionId id{};
			std::vector<std::uint8_t> message;
			float sinceSend = 0.0f;
			int attempts = 0; // number of times `message` has been sent so far
		};

		enum class Phase
		{
			Allocate,
			Refresh,
		};

		struct PeerBinding
		{
			Endpoint peer{};

			bool permissionActive = false;
			float permissionRemaining = 0.0f; // seconds until the RFC 5766 s8 300s permission lapses
			float permissionCooldown = 0.0f;  // seconds before CreatePermission may be retried, after giving up once
			PendingExchange permissionPending;

			bool channelBound = false;
			std::uint16_t channelNumber = 0;
			float channelRemaining = 0.0f; // seconds until the RFC 5766 s11 10-minute channel binding lapses
			float channelCooldown = 0.0f;
			PendingExchange channelPending;
		};

		void SendRaw(std::span<const std::uint8_t> bytes);
		void Fail(std::string reason);

		// Resends `pending.message` on an exponential backoff in the spirit of RFC 5389 s7.2.1
		// and reports whether it just gave up - true only on the call where the bounded number
		// of sends was reached with nothing answering it. The caller decides what giving up
		// means for that particular exchange.
		bool TickPending(PendingExchange& pending, float deltaSeconds);

		[[nodiscard]] std::vector<std::uint8_t> BuildAllocate(const stun::TransactionId& id, bool authenticated) const;
		[[nodiscard]] std::vector<std::uint8_t> BuildRefresh(const stun::TransactionId& id, std::uint32_t lifetimeSeconds) const;
		[[nodiscard]] std::vector<std::uint8_t> BuildCreatePermission(const stun::TransactionId& id, const Endpoint& peer) const;
		[[nodiscard]] std::vector<std::uint8_t> BuildChannelBind(const stun::TransactionId& id, std::uint16_t channel, const Endpoint& peer) const;
		[[nodiscard]] static std::vector<std::uint8_t> BuildSendIndication(const Endpoint& peer, std::span<const std::byte> payload);
		[[nodiscard]] static std::vector<std::uint8_t> BuildChannelData(std::uint16_t channel, std::span<const std::byte> payload);

		// Builds a fresh (new transaction id) authenticated request for whatever m_controlPhase
		// currently is, and sends it. Used both for the retry after Allocate's first 401 and for
		// any 438 Stale Nonce on the control exchange.
		void RetryControlAuthenticated();
		void StartRefresh();
		void HandleControlResponse(const stun::MessageReader& reader);

		PeerBinding& EnsurePeer(const Endpoint& peer);
		void StartCreatePermission(PeerBinding& entry);
		void StartChannelBind(PeerBinding& entry);
		void HandlePermissionResponse(PeerBinding& entry, const stun::MessageReader& reader);
		void HandleChannelBindResponse(PeerBinding& entry, const stun::MessageReader& reader);

		Endpoint m_server{};
		std::string m_username;
		std::string m_password;
		SendFn m_send;

		State m_state = State::Idle;
		std::string m_failure;

		// Learned from the server's first 401; carried on every authenticated request after
		// that, and replaced whenever a 438 Stale Nonce hands back a fresh one.
		std::string m_realm;
		std::string m_nonce;
		int m_staleNonceRetries = 0;

		Phase m_controlPhase = Phase::Allocate;
		bool m_controlAuthenticated = false; // whether m_control.message currently in flight carries credentials
		PendingExchange m_control;

		std::optional<Endpoint> m_relayed;
		float m_lifetimeRemaining = 0.0f;      // seconds until the current allocation expires server-side
		float m_grantedLifetimeSeconds = 0.0f; // the LIFETIME the server actually granted, refreshed against

		std::vector<PeerBinding> m_peers;
		std::uint16_t m_nextChannelNumber = 0x4000; // RFC 5766 s11: channel numbers occupy 0x4000-0x7FFF
	};
} // namespace aether::net
