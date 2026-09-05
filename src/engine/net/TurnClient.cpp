#include "net/TurnClient.hpp"

#include <algorithm>
#include <utility>

#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// RFC 5389 s7.2.1: with no RTT sample yet, RTO starts here and doubles on each
		// retransmit; Rc=7 total sends (the initial one plus 6 retransmits) is what the RFC
		// recommends before a transaction is abandoned.
		constexpr float kRtoSeconds = 0.5f;
		constexpr int kMaxSendsPerTransaction = 7;

		// RFC 5766 s2.2's example LIFETIME, and what this client requests on Allocate and every
		// Refresh. The server's answer - not this constant - is what m_grantedLifetimeSeconds
		// actually tracks; a server is always free to grant less.
		constexpr float kRequestedLifetimeSeconds = 600.0f;
		// Refreshed once less than half the granted lifetime remains, never at the edge: one
		// delayed or lost Refresh still leaves the other half of the window to try again in.
		constexpr float kRefreshAtFraction = 0.5f;

		// RFC 5766 s8: a permission installed by CreatePermission always expires exactly 300
		// seconds after it was (re)installed - a protocol constant, not something requested or
		// granted. Refreshed at half that so one lost refresh still leaves 150s of margin.
		constexpr float kPermissionLifetimeSeconds = 300.0f;
		constexpr float kPermissionRefreshAtFraction = 0.5f;

		// RFC 5766 s11: a channel binding lasts 10 minutes and is refreshed the same way - by
		// sending ChannelBind again before it lapses.
		constexpr float kChannelLifetimeSeconds = 600.0f;
		constexpr float kChannelRefreshAtFraction = 0.5f;

		// How long CreatePermission/ChannelBind waits before trying again after giving up on one
		// attempt cycle (a full RFC 5389 backoff went unanswered, or the server refused
		// outright). Bounded and non-zero so a peer the relay keeps rejecting is retried at a
		// slow, steady rate instead of in a tight loop - unlike Allocate/Refresh, a single peer
		// failing to get a permission or a channel is not fatal to the whole relay.
		constexpr float kUpkeepCooldownSeconds = 5.0f;

		// RFC 5766 s14.7: REQUESTED-TRANSPORT is 4 bytes - an 8-bit protocol number in the top
		// byte (17 for UDP) followed by 24 reserved bits that MUST be zero.
		constexpr std::uint32_t kRequestedTransportUdp = 0x11000000u;
	} // namespace

	TurnClient::TurnClient(Endpoint server, std::string username, std::string password, SendFn send)
	      : m_server(server)
	      , m_username(std::move(username))
	      , m_password(std::move(password))
	      , m_send(std::move(send))
	{
	}

	void TurnClient::SendRaw(std::span<const std::uint8_t> bytes)
	{
		if (m_send)
		{
			m_send(m_server, bytes);
		}
	}

	void TurnClient::Fail(std::string reason)
	{
		m_state = State::Failed;
		m_failure = std::move(reason);
		m_control.active = false;
		m_relayed.reset();
		m_peers.clear();
		AE_WARN(LogCategory::App, "TURN client failed: {}", m_failure);
	}

	bool TurnClient::TickPending(PendingExchange& pending, float deltaSeconds)
	{
		pending.sinceSend += deltaSeconds;
		const float rto = kRtoSeconds * static_cast<float>(1 << (pending.attempts - 1));
		if (pending.sinceSend < rto)
		{
			return false;
		}
		if (pending.attempts >= kMaxSendsPerTransaction)
		{
			pending.active = false;
			return true;
		}
		pending.sinceSend = 0.0f;
		++pending.attempts;
		SendRaw(pending.message);
		return false;
	}

	std::vector<std::uint8_t> TurnClient::BuildAllocate(const stun::TransactionId& id, bool authenticated) const
	{
		stun::MessageBuilder builder(stun::Method::Allocate, stun::MessageClass::Request, id);
		builder.AddU32(stun::Attribute::RequestedTransport, kRequestedTransportUdp);
		builder.AddU32(stun::Attribute::Lifetime, static_cast<std::uint32_t>(kRequestedLifetimeSeconds));
		if (authenticated)
		{
			builder.AddText(stun::Attribute::Username, m_username);
			builder.AddText(stun::Attribute::Realm, m_realm);
			builder.AddText(stun::Attribute::Nonce, m_nonce);
			builder.AppendMessageIntegrity(stun::crypto::LongTermKey(m_username, m_realm, m_password));
		}
		const auto bytes = builder.Bytes();
		return {bytes.begin(), bytes.end()};
	}

	std::vector<std::uint8_t> TurnClient::BuildRefresh(const stun::TransactionId& id, std::uint32_t lifetimeSeconds) const
	{
		stun::MessageBuilder builder(stun::Method::Refresh, stun::MessageClass::Request, id);
		builder.AddU32(stun::Attribute::Lifetime, lifetimeSeconds);
		builder.AddText(stun::Attribute::Username, m_username);
		builder.AddText(stun::Attribute::Realm, m_realm);
		builder.AddText(stun::Attribute::Nonce, m_nonce);
		builder.AppendMessageIntegrity(stun::crypto::LongTermKey(m_username, m_realm, m_password));
		const auto bytes = builder.Bytes();
		return {bytes.begin(), bytes.end()};
	}

	std::vector<std::uint8_t> TurnClient::BuildCreatePermission(const stun::TransactionId& id, const Endpoint& peer) const
	{
		stun::MessageBuilder builder(stun::Method::CreatePermission, stun::MessageClass::Request, id);
		builder.AddXorAddress(stun::Attribute::XorPeerAddress, peer);
		builder.AddText(stun::Attribute::Username, m_username);
		builder.AddText(stun::Attribute::Realm, m_realm);
		builder.AddText(stun::Attribute::Nonce, m_nonce);
		builder.AppendMessageIntegrity(stun::crypto::LongTermKey(m_username, m_realm, m_password));
		const auto bytes = builder.Bytes();
		return {bytes.begin(), bytes.end()};
	}

	std::vector<std::uint8_t> TurnClient::BuildChannelBind(const stun::TransactionId& id, std::uint16_t channel, const Endpoint& peer) const
	{
		stun::MessageBuilder builder(stun::Method::ChannelBind, stun::MessageClass::Request, id);
		// RFC 5766 s14.1: 16-bit channel number followed by 16 reserved bits that MUST be zero.
		builder.AddU32(stun::Attribute::ChannelNumber, static_cast<std::uint32_t>(channel) << 16);
		builder.AddXorAddress(stun::Attribute::XorPeerAddress, peer);
		builder.AddText(stun::Attribute::Username, m_username);
		builder.AddText(stun::Attribute::Realm, m_realm);
		builder.AddText(stun::Attribute::Nonce, m_nonce);
		builder.AppendMessageIntegrity(stun::crypto::LongTermKey(m_username, m_realm, m_password));
		const auto bytes = builder.Bytes();
		return {bytes.begin(), bytes.end()};
	}

	std::vector<std::uint8_t> TurnClient::BuildSendIndication(const Endpoint& peer, std::span<const std::byte> payload)
	{
		// Indications are not authenticated (RFC 5766 s10.1): the allocation itself is already
		// bound to this client's 5-tuple, and a server never challenges one with 401 the way it
		// does a request - so there is no credential exchange to carry here.
		stun::MessageBuilder builder(stun::Method::Send, stun::MessageClass::Indication, stun::MakeTransactionId());
		builder.AddXorAddress(stun::Attribute::XorPeerAddress, peer);
		builder.AddBytes(stun::Attribute::Data, payload);
		const auto bytes = builder.Bytes();
		return {bytes.begin(), bytes.end()};
	}

	std::vector<std::uint8_t> TurnClient::BuildChannelData(std::uint16_t channel, std::span<const std::byte> payload)
	{
		std::vector<std::uint8_t> out;
		out.reserve(stun::kChannelDataHeaderSize + payload.size());
		out.push_back(static_cast<std::uint8_t>(channel >> 8));
		out.push_back(static_cast<std::uint8_t>(channel & 0xFFu));
		out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFFu));
		out.push_back(static_cast<std::uint8_t>(payload.size() & 0xFFu));
		const auto* p = reinterpret_cast<const std::uint8_t*>(payload.data());
		out.insert(out.end(), p, p + payload.size());
		return out;
	}

	void TurnClient::BeginAllocate()
	{
		m_state = State::Allocating;
		m_failure.clear();
		m_relayed.reset();
		m_realm.clear();
		m_nonce.clear();
		m_staleNonceRetries = 0;
		m_lifetimeRemaining = 0.0f;
		m_grantedLifetimeSeconds = 0.0f;
		m_peers.clear();

		m_controlPhase = Phase::Allocate;
		m_controlAuthenticated = false;
		m_control.id = stun::MakeTransactionId();
		m_control.message = BuildAllocate(m_control.id, false);
		m_control.attempts = 1;
		m_control.sinceSend = 0.0f;
		m_control.active = true;
		SendRaw(m_control.message);
	}

	void TurnClient::Release()
	{
		if (m_state != State::Allocating && m_state != State::Allocated)
		{
			return; // nothing outstanding to give up
		}
		if (m_state == State::Allocated)
		{
			const auto id = stun::MakeTransactionId();
			SendRaw(BuildRefresh(id, 0));
		}
		m_control.active = false;
		m_state = State::Released;
		m_relayed.reset();
		m_peers.clear();
		AE_INFO(LogCategory::App, "TURN client: released the allocation.");
	}

	void TurnClient::RetryControlAuthenticated()
	{
		m_controlAuthenticated = true;
		m_control.id = stun::MakeTransactionId();
		m_control.message = m_controlPhase == Phase::Allocate ? BuildAllocate(m_control.id, true) : BuildRefresh(m_control.id, static_cast<std::uint32_t>(kRequestedLifetimeSeconds));
		m_control.attempts = 1;
		m_control.sinceSend = 0.0f;
		m_control.active = true;
		SendRaw(m_control.message);
	}

	void TurnClient::StartRefresh()
	{
		m_controlPhase = Phase::Refresh;
		m_controlAuthenticated = true; // credentials are already known from a successful Allocate
		m_control.id = stun::MakeTransactionId();
		m_control.message = BuildRefresh(m_control.id, static_cast<std::uint32_t>(kRequestedLifetimeSeconds));
		m_control.attempts = 1;
		m_control.sinceSend = 0.0f;
		m_control.active = true;
		SendRaw(m_control.message);
	}

	void TurnClient::HandleControlResponse(const stun::MessageReader& reader)
	{
		if (!m_control.active || reader.GetTransactionId() != m_control.id)
		{
			return; // stale, unmatched, or nothing outstanding - never trusted regardless
		}

		if (reader.GetClass() == stun::MessageClass::SuccessResponse)
		{
			// Success only ever follows an authenticated request: the anonymous first Allocate
			// attempt can only ever be challenged, never granted, so a key is always available
			// here, and this response is not trusted until it checks out against it.
			const auto key = stun::crypto::LongTermKey(m_username, m_realm, m_password);
			if (!reader.VerifyMessageIntegrity(key))
			{
				AE_WARN(LogCategory::App, "TURN: a success response failed MESSAGE-INTEGRITY; ignored, still waiting");
				return;
			}
			const auto lifetime = reader.U32(stun::Attribute::Lifetime);
			if (!lifetime.has_value())
			{
				AE_WARN(LogCategory::App, "TURN: a success response carried no LIFETIME; ignored, still waiting");
				return;
			}
			if (m_controlPhase == Phase::Allocate)
			{
				const auto relayed = reader.XorAddress(stun::Attribute::XorRelayedAddress);
				if (!relayed.has_value())
				{
					AE_WARN(LogCategory::App, "TURN: Allocate succeeded with no XOR-RELAYED-ADDRESS; ignored, still waiting");
					return;
				}
				m_relayed = relayed;
				m_state = State::Allocated;
				AE_INFO(LogCategory::App, "TURN client: allocated a relayed endpoint.");
			}
			m_grantedLifetimeSeconds = static_cast<float>(*lifetime);
			m_lifetimeRemaining = m_grantedLifetimeSeconds;
			m_control.active = false;
			return;
		}

		if (reader.GetClass() != stun::MessageClass::ErrorResponse)
		{
			return; // a Request/Indication echo of our own method would be nonsensical; ignored
		}

		const auto error = reader.ErrorCode();
		if (!error.has_value())
		{
			AE_WARN(LogCategory::App, "TURN: an error response carried no parseable ERROR-CODE; ignored, still waiting");
			return;
		}
		const auto& [code, reasonText] = *error;

		if (code == 401)
		{
			if (m_controlAuthenticated)
			{
				// The retry already carried USERNAME/REALM/NONCE/MESSAGE-INTEGRITY and was
				// still rejected: retrying again with the same credentials cannot succeed, so
				// this is reported as what it is rather than retried into a loop.
				Fail("the TURN server rejected the configured username/password");
				return;
			}
			const auto realm = reader.Text(stun::Attribute::Realm);
			const auto nonce = reader.Text(stun::Attribute::Nonce);
			if (!realm.has_value() || !nonce.has_value())
			{
				Fail("the TURN server sent 401 Unauthorized without REALM/NONCE");
				return;
			}
			m_realm.assign(*realm);
			m_nonce.assign(*nonce);
			RetryControlAuthenticated();
			return;
		}

		if (code == 438)
		{
			if (++m_staleNonceRetries > kMaxSendsPerTransaction)
			{
				Fail("the TURN server kept rejecting every NONCE as stale");
				return;
			}
			const auto nonce = reader.Text(stun::Attribute::Nonce);
			if (!nonce.has_value())
			{
				Fail("the TURN server sent 438 Stale Nonce without a replacement NONCE");
				return;
			}
			m_nonce.assign(*nonce);
			RetryControlAuthenticated();
			return;
		}

		Fail("the TURN server refused " + std::string(m_controlPhase == Phase::Allocate ? "Allocate" : "Refresh") + ": " + std::to_string(code) + " " + std::string(reasonText));
	}

	TurnClient::PeerBinding& TurnClient::EnsurePeer(const Endpoint& peer)
	{
		const auto it = std::ranges::find_if(m_peers, [&](const PeerBinding& p) { return p.peer == peer; });
		if (it != m_peers.end())
		{
			return *it;
		}
		m_peers.push_back(PeerBinding{});
		PeerBinding& entry = m_peers.back();
		entry.peer = peer;
		StartCreatePermission(entry);
		return entry;
	}

	void TurnClient::StartCreatePermission(PeerBinding& entry)
	{
		entry.permissionPending.id = stun::MakeTransactionId();
		entry.permissionPending.message = BuildCreatePermission(entry.permissionPending.id, entry.peer);
		entry.permissionPending.attempts = 1;
		entry.permissionPending.sinceSend = 0.0f;
		entry.permissionPending.active = true;
		SendRaw(entry.permissionPending.message);
	}

	void TurnClient::StartChannelBind(PeerBinding& entry)
	{
		if (entry.channelNumber == 0)
		{
			// Numbers wrap, but a wrapped number must never land on one some other
			// binding still holds: OnDatagram's lookup hands ChannelData to the FIRST
			// matching bound channel, so a reuse would attribute relayed traffic to
			// the OLD peer.
			std::uint16_t candidate = m_nextChannelNumber;
			for (unsigned tries = 0; tries <= 0x7FFFu - 0x4000u; ++tries)
			{
				const bool taken = std::ranges::any_of(m_peers,
				        [&](const PeerBinding& p) { return p.channelBound && p.channelNumber == candidate; });
				if (!taken)
				{
					entry.channelNumber = candidate;
					m_nextChannelNumber = candidate < 0x7FFFu ? static_cast<std::uint16_t>(candidate + 1) : 0x4000u;
					break;
				}
				candidate = candidate < 0x7FFFu ? static_cast<std::uint16_t>(candidate + 1) : 0x4000u;
			}
			if (entry.channelNumber == 0)
			{
				// Every number is held by a live binding - keep this peer on Send
				// indications rather than bind onto an occupied channel.
				entry.channelCooldown = kUpkeepCooldownSeconds;
				return;
			}
		}
		entry.channelPending.id = stun::MakeTransactionId();
		entry.channelPending.message = BuildChannelBind(entry.channelPending.id, entry.channelNumber, entry.peer);
		entry.channelPending.attempts = 1;
		entry.channelPending.sinceSend = 0.0f;
		entry.channelPending.active = true;
		SendRaw(entry.channelPending.message);
	}

	void TurnClient::HandlePermissionResponse(PeerBinding& entry, const stun::MessageReader& reader)
	{
		if (!entry.permissionPending.active || reader.GetTransactionId() != entry.permissionPending.id)
		{
			return;
		}

		if (reader.GetClass() == stun::MessageClass::SuccessResponse)
		{
			const auto key = stun::crypto::LongTermKey(m_username, m_realm, m_password);
			if (!reader.VerifyMessageIntegrity(key))
			{
				AE_WARN(LogCategory::App, "TURN: CreatePermission success response failed MESSAGE-INTEGRITY; ignored, still waiting");
				return;
			}
			entry.permissionActive = true;
			entry.permissionRemaining = kPermissionLifetimeSeconds;
			entry.permissionPending.active = false;
			return;
		}
		if (reader.GetClass() != stun::MessageClass::ErrorResponse)
		{
			return;
		}

		// A 438/error NONCE is adopted only from a response whose MESSAGE-INTEGRITY
		// verifies: by this point the key is known and every conformant server reply
		// is integrity-protected (RFC 5389 s10.2.3), so an unverified one is an
		// on-path spoof racing the real reply - adopting its nonce would wedge the
		// retry loop against the genuine server. Keep waiting instead.
		const auto key = stun::crypto::LongTermKey(m_username, m_realm, m_password);
		if (!reader.VerifyMessageIntegrity(key))
		{
			AE_WARN(LogCategory::App, "TURN: unverified CreatePermission error response ignored; still waiting");
			return;
		}
		// A fresh NONCE (e.g. from a 438) is adopted regardless of the error code, so the next
		// attempt - after the cooldown below - has whatever the server most recently issued.
		if (const auto nonce = reader.Text(stun::Attribute::Nonce); nonce.has_value())
		{
			m_nonce.assign(*nonce);
		}
		AE_WARN(LogCategory::App, "TURN: CreatePermission for a peer was refused; will retry after a short cooldown");
		entry.permissionPending.active = false;
		entry.permissionCooldown = kUpkeepCooldownSeconds;
	}

	void TurnClient::HandleChannelBindResponse(PeerBinding& entry, const stun::MessageReader& reader)
	{
		if (!entry.channelPending.active || reader.GetTransactionId() != entry.channelPending.id)
		{
			return;
		}

		if (reader.GetClass() == stun::MessageClass::SuccessResponse)
		{
			const auto key = stun::crypto::LongTermKey(m_username, m_realm, m_password);
			if (!reader.VerifyMessageIntegrity(key))
			{
				AE_WARN(LogCategory::App, "TURN: ChannelBind success response failed MESSAGE-INTEGRITY; ignored, still waiting");
				return;
			}
			entry.channelBound = true;
			entry.channelRemaining = kChannelLifetimeSeconds;
			entry.channelPending.active = false;
			AE_INFO(LogCategory::App, "TURN client: a channel is bound; traffic to that peer now rides as ChannelData.");
			return;
		}
		if (reader.GetClass() != stun::MessageClass::ErrorResponse)
		{
			return;
		}

		// Same rule as CreatePermission: post-auth, an error response that does not
		// verify is a spoof racing the real reply, not the server's new nonce.
		{
			const auto key = stun::crypto::LongTermKey(m_username, m_realm, m_password);
			if (!reader.VerifyMessageIntegrity(key))
			{
				AE_WARN(LogCategory::App, "TURN: unverified ChannelBind error response ignored; still waiting");
				return;
			}
		}
		if (const auto nonce = reader.Text(stun::Attribute::Nonce); nonce.has_value())
		{
			m_nonce.assign(*nonce);
		}
		AE_WARN(LogCategory::App, "TURN: ChannelBind for a peer was refused; continuing with Send indications, will retry after a short cooldown");
		entry.channelPending.active = false;
		entry.channelCooldown = kUpkeepCooldownSeconds;
	}

	void TurnClient::Tick(float deltaSeconds)
	{
		if (m_control.active && TickPending(m_control, deltaSeconds))
		{
			if (m_controlPhase == Phase::Allocate)
			{
				Fail("no response from the TURN relay - check turnHost/turnPort, or that outbound UDP can reach it");
			}
			else
			{
				Fail("no response from the TURN relay while refreshing the allocation; it may have expired");
			}
		}

		if (m_state != State::Allocated)
		{
			return;
		}

		m_lifetimeRemaining -= deltaSeconds;
		if (!m_control.active && m_lifetimeRemaining <= m_grantedLifetimeSeconds * (1.0f - kRefreshAtFraction))
		{
			StartRefresh();
		}

		for (PeerBinding& entry: m_peers)
		{
			if (entry.permissionActive)
			{
				entry.permissionRemaining -= deltaSeconds;
			}
			if (entry.permissionPending.active)
			{
				if (TickPending(entry.permissionPending, deltaSeconds))
				{
					AE_WARN(LogCategory::App, "TURN: CreatePermission for a peer did not get answered; will retry after a short cooldown");
					entry.permissionCooldown = kUpkeepCooldownSeconds;
				}
			}
			else if (entry.permissionCooldown > 0.0f)
			{
				entry.permissionCooldown -= deltaSeconds;
			}
			else if (!entry.permissionActive || entry.permissionRemaining <= kPermissionLifetimeSeconds * (1.0f - kPermissionRefreshAtFraction))
			{
				StartCreatePermission(entry);
			}

			if (entry.channelBound)
			{
				entry.channelRemaining -= deltaSeconds;
			}
			if (entry.channelPending.active)
			{
				if (TickPending(entry.channelPending, deltaSeconds))
				{
					AE_WARN(LogCategory::App, "TURN: ChannelBind for a peer did not get answered; will retry after a short cooldown");
					entry.channelCooldown = kUpkeepCooldownSeconds;
				}
			}
			else if (entry.channelCooldown > 0.0f)
			{
				entry.channelCooldown -= deltaSeconds;
			}
			else if (entry.permissionActive && (!entry.channelBound || entry.channelRemaining <= kChannelLifetimeSeconds * (1.0f - kChannelRefreshAtFraction)))
			{
				StartChannelBind(entry);
			}
		}
	}

	TurnClient::Delivery TurnClient::OnDatagram(const Endpoint& from, std::span<const std::byte> data)
	{
		if (!(from == m_server))
		{
			return {}; // not ours: consumed=false, so the caller keeps looking elsewhere for it
		}

		if (stun::LooksLikeChannelData(data))
		{
			Delivery out;
			out.consumed = true;
			if (data.size() < stun::kChannelDataHeaderSize)
			{
				return out; // truncated header; dropped rather than read out of bounds
			}
			const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
			const auto channel = static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
			const auto length = static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[2]) << 8) | p[3]);
			if (static_cast<std::size_t>(stun::kChannelDataHeaderSize) + length > data.size())
			{
				return out; // claims more application data than the datagram actually holds
			}
			const auto it = std::ranges::find_if(m_peers, [&](const PeerBinding& p2) { return p2.channelBound && p2.channelNumber == channel; });
			if (it == m_peers.end())
			{
				return out; // an unbound or unknown channel number; dropped, never guessed at
			}
			out.peer = it->peer;
			out.payload = data.subspan(stun::kChannelDataHeaderSize, length);
			return out;
		}

		const auto reader = stun::MessageReader::Parse(data);
		if (!reader.has_value())
		{
			return {true, std::nullopt, {}}; // garbage from the relay's own address: dropped, never ENet's
		}

		Delivery out;
		out.consumed = true;
		switch (reader->GetMethod())
		{
			case stun::Method::Allocate:
			case stun::Method::Refresh:
				HandleControlResponse(*reader);
				break;

			case stun::Method::CreatePermission:
			{
				const auto it = std::ranges::find_if(m_peers, [&](const PeerBinding& p) { return p.permissionPending.active && p.permissionPending.id == reader->GetTransactionId(); });
				if (it != m_peers.end())
				{
					HandlePermissionResponse(*it, *reader);
				}
				break;
			}

			case stun::Method::ChannelBind:
			{
				const auto it = std::ranges::find_if(m_peers, [&](const PeerBinding& p) { return p.channelPending.active && p.channelPending.id == reader->GetTransactionId(); });
				if (it != m_peers.end())
				{
					HandleChannelBindResponse(*it, *reader);
				}
				break;
			}

			case stun::Method::Data:
				if (reader->GetClass() == stun::MessageClass::Indication)
				{
					const auto peer = reader->XorAddress(stun::Attribute::XorPeerAddress);
					const auto payload = reader->Find(stun::Attribute::Data);
					// RFC 5766 s10.4 requires both attributes; a Data indication missing either
					// is malformed and dropped rather than delivered with a guessed source.
					if (peer.has_value() && payload.has_value())
					{
						out.peer = *peer;
						out.payload = *payload;
					}
				}
				break;

			case stun::Method::Binding:
			case stun::Method::Send:
			default:
				break; // never expected from a relay in this direction; ignored, not acted on
		}
		return out;
	}

	void TurnClient::PermitPeer(const Endpoint& peer)
	{
		if (m_state != State::Allocated)
		{
			return;
		}
		EnsurePeer(peer);
	}

	void TurnClient::SendToPeer(const Endpoint& peer, std::span<const std::byte> payload)
	{
		if (m_state != State::Allocated)
		{
			return; // no relay to send through yet; dropped like any other UDP loss
		}
		if (payload.size() > 0xFFFFu)
		{
			AE_WARN(LogCategory::App, "TURN: dropped an outgoing datagram too large for ChannelData's 16-bit length ({} bytes)", payload.size());
			return;
		}
		PeerBinding& entry = EnsurePeer(peer);
		if (entry.channelBound)
		{
			SendRaw(BuildChannelData(entry.channelNumber, payload));
		}
		else
		{
			SendRaw(BuildSendIndication(peer, payload));
		}
	}
} // namespace aether::net
