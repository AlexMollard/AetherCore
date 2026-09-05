#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "net/TurnClient.hpp"

using namespace aether::net;

namespace
{
	// RFC 5389 s15.6: ERROR-CODE packs a class digit (top 3 bits of the 3rd value byte)
	// and a two-digit number (4th byte); MessageReader::ErrorCode() reassembles them as
	// class*100+number, so the fake server below packs the same way rather than writing
	// the code as one plain integer.
	std::uint32_t PackErrorCode(int code)
	{
		return (static_cast<std::uint32_t>(code / 100) << 8) | static_cast<std::uint32_t>(code % 100);
	}

	stun::Endpoint MakeEndpoint(std::uint32_t address, std::uint16_t port)
	{
		return stun::Endpoint{address, port};
	}

	std::span<const std::byte> AsBytes(const std::vector<std::uint8_t>& v)
	{
		return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
	}

	const std::string kUsername = "gamer1";
	const std::string kPassword = "hunter2";
	const std::string kRealm = "aethercore.example";
	const stun::Endpoint kServer = MakeEndpoint(0x0A000001u, 3478); // 10.0.0.1:3478
	const stun::Endpoint kPeer = MakeEndpoint(0x0A000002u, 40000);  // 10.0.0.2:40000
	const stun::Endpoint kRelayed = MakeEndpoint(0xC0000201u, 55555);

	using SentLog = std::vector<std::vector<std::uint8_t>>;

	// Every send this fake server's client makes is checked to have gone to the server
	// address the client was configured with - TurnClient must never address anything
	// else, since nothing but the relay is meant to receive its traffic.
	TurnClient MakeClient(SentLog& sent, const std::string& username = kUsername, const std::string& password = kPassword)
	{
		return TurnClient(kServer, username, password, [&sent](const stun::Endpoint& to, std::span<const std::uint8_t> bytes) {
			CHECK(to == kServer);
			sent.emplace_back(bytes.begin(), bytes.end());
		});
	}

	// Drives BeginAllocate() through the standard 401 -> authenticated retry -> success
	// handshake so tests that only care about post-allocation behaviour do not have to
	// repeat it. Leaves the client Allocated with `grantedLifetime` seconds.
	void DriveToAllocated(TurnClient& client, SentLog& sent, std::uint32_t grantedLifetime = 600)
	{
		client.BeginAllocate();
		REQUIRE(sent.size() == 1);
		{
			const auto reader = stun::MessageReader::Parse(AsBytes(sent.back()));
			REQUIRE(reader.has_value());
			stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::ErrorResponse, reader->GetTransactionId());
			resp.AddU32(stun::Attribute::ErrorCode, PackErrorCode(401));
			resp.AddText(stun::Attribute::Realm, kRealm);
			resp.AddText(stun::Attribute::Nonce, "nonce-1");
			CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
		}
		REQUIRE(sent.size() == 2);
		{
			const auto reader = stun::MessageReader::Parse(AsBytes(sent.back()));
			REQUIRE(reader.has_value());
			const auto key = stun::crypto::LongTermKey(kUsername, kRealm, kPassword);
			REQUIRE(reader->VerifyMessageIntegrity(key));
			stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::SuccessResponse, reader->GetTransactionId());
			resp.AddU32(stun::Attribute::Lifetime, grantedLifetime);
			resp.AddXorAddress(stun::Attribute::XorRelayedAddress, kRelayed);
			resp.AppendMessageIntegrity(key);
			CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
		}
		REQUIRE(client.GetState() == TurnClient::State::Allocated);
	}

	// Answers the most recently sent request with an authenticated success carrying no
	// attributes beyond MESSAGE-INTEGRITY - which is all CreatePermission and ChannelBind
	// success responses ever carry (RFC 5766 s9.2, s11.2).
	void RespondSuccess(TurnClient& client, stun::Method method, const std::vector<std::uint8_t>& request)
	{
		const auto reader = stun::MessageReader::Parse(AsBytes(request));
		REQUIRE(reader.has_value());
		REQUIRE(reader->GetMethod() == method);
		stun::MessageBuilder resp(method, stun::MessageClass::SuccessResponse, reader->GetTransactionId());
		resp.AppendMessageIntegrity(stun::crypto::LongTermKey(kUsername, kRealm, kPassword));
		CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	}
} // namespace

TEST_CASE("Allocate's unauthenticated probe is challenged with 401, and the authenticated retry carries genuine MESSAGE-INTEGRITY")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);

	client.BeginAllocate();
	CHECK(client.GetState() == TurnClient::State::Allocating);
	REQUIRE(sent.size() == 1);

	const auto probe = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(probe.has_value());
	CHECK(probe->GetMethod() == stun::Method::Allocate);
	CHECK(probe->GetClass() == stun::MessageClass::Request);
	CHECK_FALSE(probe->Find(stun::Attribute::Username).has_value());
	CHECK_FALSE(probe->Find(stun::Attribute::MessageIntegrity).has_value());
	const auto transport = probe->U32(stun::Attribute::RequestedTransport);
	REQUIRE(transport.has_value());
	CHECK(*transport == 0x11000000u); // RFC 5766 s14.7: 17 (UDP) in the top byte, rest reserved

	stun::MessageBuilder challenge(stun::Method::Allocate, stun::MessageClass::ErrorResponse, probe->GetTransactionId());
	challenge.AddU32(stun::Attribute::ErrorCode, PackErrorCode(401));
	challenge.AddText(stun::Attribute::Realm, kRealm);
	challenge.AddText(stun::Attribute::Nonce, "nonce-1");
	CHECK(client.OnDatagram(kServer, std::as_bytes(challenge.Bytes())).consumed);

	CHECK(client.GetState() == TurnClient::State::Allocating);
	REQUIRE(sent.size() == 2);

	const auto retry = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(retry.has_value());
	CHECK(retry->GetTransactionId() != probe->GetTransactionId()); // 401 starts a NEW transaction, not a retransmit
	const auto username = retry->Text(stun::Attribute::Username);
	const auto realm = retry->Text(stun::Attribute::Realm);
	const auto nonce = retry->Text(stun::Attribute::Nonce);
	REQUIRE(username.has_value());
	REQUIRE(realm.has_value());
	REQUIRE(nonce.has_value());
	CHECK(*username == kUsername);
	CHECK(*realm == kRealm);
	CHECK(*nonce == "nonce-1");

	// The real proof of interop-grade auth: recompute the key exactly as a real server
	// would and verify the client's own HMAC against it.
	const auto key = stun::crypto::LongTermKey(kUsername, kRealm, kPassword);
	CHECK(retry->VerifyMessageIntegrity(key));
	const auto wrongKey = stun::crypto::LongTermKey(kUsername, kRealm, "not-the-password");
	CHECK_FALSE(retry->VerifyMessageIntegrity(wrongKey));
}

TEST_CASE("A successful Allocate exposes the relayed endpoint from XOR-RELAYED-ADDRESS")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	CHECK(client.GetState() == TurnClient::State::Allocated);
	const auto relayed = client.RelayedEndpoint();
	REQUIRE(relayed.has_value());
	CHECK(relayed->address == kRelayed.address);
	CHECK(relayed->port == kRelayed.port);
}

TEST_CASE("438 Stale Nonce is handled by adopting the new nonce and retrying, not treated as fatal")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);

	client.BeginAllocate();
	REQUIRE(sent.size() == 1);
	const auto probeId = stun::MessageReader::Parse(AsBytes(sent.back()))->GetTransactionId();
	{
		stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::ErrorResponse, probeId);
		resp.AddU32(stun::Attribute::ErrorCode, PackErrorCode(401));
		resp.AddText(stun::Attribute::Realm, kRealm);
		resp.AddText(stun::Attribute::Nonce, "nonce-old");
		CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	}
	REQUIRE(sent.size() == 2);

	// The server rotated its nonce between the challenge and this reply - real servers do
	// this periodically, and a client that gives up here would die in production.
	const auto firstRetry = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(firstRetry.has_value());
	{
		stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::ErrorResponse, firstRetry->GetTransactionId());
		resp.AddU32(stun::Attribute::ErrorCode, PackErrorCode(438));
		resp.AddText(stun::Attribute::Realm, kRealm);
		resp.AddText(stun::Attribute::Nonce, "nonce-fresh");
		CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	}

	CHECK(client.GetState() == TurnClient::State::Allocating); // not Failed
	REQUIRE(sent.size() == 3);
	const auto secondRetry = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(secondRetry.has_value());
	CHECK(secondRetry->GetTransactionId() != firstRetry->GetTransactionId()); // a new transaction, per RFC 5766 s7.3
	const auto nonce = secondRetry->Text(stun::Attribute::Nonce);
	REQUIRE(nonce.has_value());
	CHECK(*nonce == "nonce-fresh");
	const auto key = stun::crypto::LongTermKey(kUsername, kRealm, kPassword);
	CHECK(secondRetry->VerifyMessageIntegrity(key));

	// And it actually completes from there.
	stun::MessageBuilder success(stun::Method::Allocate, stun::MessageClass::SuccessResponse, secondRetry->GetTransactionId());
	success.AddU32(stun::Attribute::Lifetime, 600u);
	success.AddXorAddress(stun::Attribute::XorRelayedAddress, kRelayed);
	success.AppendMessageIntegrity(key);
	CHECK(client.OnDatagram(kServer, std::as_bytes(success.Bytes())).consumed);
	CHECK(client.GetState() == TurnClient::State::Allocated);
}

TEST_CASE("Repeated 401 after credentials are presented fails deterministically with a distinct reason, not an infinite loop")
{
	SentLog sent;
	TurnClient client = MakeClient(sent, kUsername, "totally-wrong-password");

	client.BeginAllocate();
	REQUIRE(sent.size() == 1);
	const auto probeId = stun::MessageReader::Parse(AsBytes(sent.back()))->GetTransactionId();
	{
		stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::ErrorResponse, probeId);
		resp.AddU32(stun::Attribute::ErrorCode, PackErrorCode(401));
		resp.AddText(stun::Attribute::Realm, kRealm);
		resp.AddText(stun::Attribute::Nonce, "nonce-a");
		CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	}
	CHECK(client.GetState() == TurnClient::State::Allocating);
	REQUIRE(sent.size() == 2);

	// A real server rejects the retry too, because the password is wrong - it does not
	// matter that the client presented credentials in the RFC-correct shape.
	const auto retry = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(retry.has_value());
	{
		stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::ErrorResponse, retry->GetTransactionId());
		resp.AddU32(stun::Attribute::ErrorCode, PackErrorCode(401));
		resp.AddText(stun::Attribute::Realm, kRealm);
		resp.AddText(stun::Attribute::Nonce, "nonce-b");
		CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	}

	CHECK(client.GetState() == TurnClient::State::Failed);
	CHECK_FALSE(client.FailureReason().empty());
	CHECK(client.FailureReason().find("username/password") != std::string::npos);
	CHECK(sent.size() == 2); // exactly two requests - no retry storm

	// Nothing further is ever sent, even if the caller keeps ticking.
	client.Tick(500.0f);
	CHECK(sent.size() == 2);
	CHECK(client.GetState() == TurnClient::State::Failed);
}

TEST_CASE("CreatePermission and ChannelBind install for a peer, and traffic switches to ChannelData once bound")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	sent.clear();
	client.PermitPeer(kPeer);
	REQUIRE(sent.size() == 1);
	{
		const auto reader = stun::MessageReader::Parse(AsBytes(sent.back()));
		REQUIRE(reader.has_value());
		CHECK(reader->GetMethod() == stun::Method::CreatePermission);
		const auto peerAttr = reader->XorAddress(stun::Attribute::XorPeerAddress);
		REQUIRE(peerAttr.has_value());
		CHECK(peerAttr->address == kPeer.address);
		CHECK(peerAttr->port == kPeer.port);
	}
	RespondSuccess(client, stun::Method::CreatePermission, sent.back());

	// ChannelBind starts on the next upkeep pass, once the permission is confirmed.
	client.Tick(0.01f);
	REQUIRE(sent.size() == 2);
	const auto bindReader = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(bindReader.has_value());
	CHECK(bindReader->GetMethod() == stun::Method::ChannelBind);
	const auto channelAttr = bindReader->U32(stun::Attribute::ChannelNumber);
	REQUIRE(channelAttr.has_value());
	const auto channel = static_cast<std::uint16_t>((*channelAttr) >> 16);
	CHECK(channel >= 0x4000);
	CHECK(channel <= 0x7FFF);
	CHECK((*channelAttr & 0xFFFFu) == 0u); // RFC 5766 s14.1: the low 16 bits are reserved and must be zero
	const auto peerAttr = bindReader->XorAddress(stun::Attribute::XorPeerAddress);
	REQUIRE(peerAttr.has_value());
	CHECK(peerAttr->address == kPeer.address);
	CHECK(peerAttr->port == kPeer.port);

	RespondSuccess(client, stun::Method::ChannelBind, sent.back());

	// A send to this peer now rides as 4-byte ChannelData rather than a Send indication.
	sent.clear();
	const std::vector<std::byte> payload{std::byte{0xAA}, std::byte{0xBB}};
	client.SendToPeer(kPeer, payload);
	REQUIRE(sent.size() == 1);
	CHECK(sent.back().size() == stun::kChannelDataHeaderSize + payload.size());
	CHECK(stun::LooksLikeChannelData(AsBytes(sent.back())));
}

TEST_CASE("Application payload round-trips through the relay: out as a Send indication before a channel binds, in as Data/ChannelData")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	sent.clear();
	const std::vector<std::byte> outbound{std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
	client.SendToPeer(kPeer, outbound); // implicitly calls PermitPeer first
	REQUIRE(sent.size() == 2);          // CreatePermission, then the indication itself

	const auto indicationReader = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(indicationReader.has_value());
	CHECK(indicationReader->GetMethod() == stun::Method::Send);
	CHECK(indicationReader->GetClass() == stun::MessageClass::Indication);
	const auto sentPeer = indicationReader->XorAddress(stun::Attribute::XorPeerAddress);
	REQUIRE(sentPeer.has_value());
	CHECK(sentPeer->address == kPeer.address);
	CHECK(sentPeer->port == kPeer.port);
	const auto sentData = indicationReader->Find(stun::Attribute::Data);
	REQUIRE(sentData.has_value());
	REQUIRE(sentData->size() == outbound.size());
	CHECK(std::equal(sentData->begin(), sentData->end(), outbound.begin()));

	// Inbound: the relay tells the client a peer sent it something, via a Data indication.
	const std::vector<std::byte> inbound{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
	stun::MessageBuilder dataIndication(stun::Method::Data, stun::MessageClass::Indication, stun::MakeTransactionId());
	dataIndication.AddXorAddress(stun::Attribute::XorPeerAddress, kPeer);
	dataIndication.AddBytes(stun::Attribute::Data, inbound);
	const auto delivery = client.OnDatagram(kServer, std::as_bytes(dataIndication.Bytes()));

	CHECK(delivery.consumed);
	REQUIRE(delivery.peer.has_value());
	CHECK(delivery.peer->address == kPeer.address);
	CHECK(delivery.peer->port == kPeer.port);
	REQUIRE(delivery.payload.size() == inbound.size());
	CHECK(std::equal(delivery.payload.begin(), delivery.payload.end(), inbound.begin()));
}

TEST_CASE("Inbound ChannelData unwraps to the bound peer and exact payload bytes")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	sent.clear();
	client.PermitPeer(kPeer);
	REQUIRE(sent.size() == 1);
	RespondSuccess(client, stun::Method::CreatePermission, sent.back());
	client.Tick(0.01f);
	REQUIRE(sent.size() == 2);
	const auto bindReader = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(bindReader.has_value());
	const auto channel = static_cast<std::uint16_t>((*bindReader->U32(stun::Attribute::ChannelNumber)) >> 16);
	RespondSuccess(client, stun::Method::ChannelBind, sent.back());

	const std::vector<std::byte> inbound{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}, std::byte{0x55}};
	std::vector<std::uint8_t> channelData;
	channelData.push_back(static_cast<std::uint8_t>(channel >> 8));
	channelData.push_back(static_cast<std::uint8_t>(channel & 0xFF));
	channelData.push_back(static_cast<std::uint8_t>(inbound.size() >> 8));
	channelData.push_back(static_cast<std::uint8_t>(inbound.size() & 0xFF));
	for (const std::byte b: inbound)
	{
		channelData.push_back(static_cast<std::uint8_t>(b));
	}

	const auto delivery = client.OnDatagram(kServer, AsBytes(channelData));
	CHECK(delivery.consumed);
	REQUIRE(delivery.peer.has_value());
	CHECK(delivery.peer->address == kPeer.address);
	CHECK(delivery.peer->port == kPeer.port);
	REQUIRE(delivery.payload.size() == inbound.size());
	CHECK(std::equal(delivery.payload.begin(), delivery.payload.end(), inbound.begin()));
}

TEST_CASE("Refresh fires before the granted lifetime expires, not at the edge")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent, 20); // short lifetime so the test does not need real seconds

	sent.clear();
	client.Tick(11.0f); // > half of the granted 20s
	const auto refreshIt = std::ranges::find_if(sent, [](const std::vector<std::uint8_t>& bytes) {
		const auto r = stun::MessageReader::Parse(AsBytes(bytes));
		return r.has_value() && r->GetMethod() == stun::Method::Refresh && r->GetClass() == stun::MessageClass::Request;
	});
	REQUIRE(refreshIt != sent.end());

	const auto reader = stun::MessageReader::Parse(AsBytes(*refreshIt));
	REQUIRE(reader.has_value());
	const auto key = stun::crypto::LongTermKey(kUsername, kRealm, kPassword);
	CHECK(reader->VerifyMessageIntegrity(key));

	stun::MessageBuilder resp(stun::Method::Refresh, stun::MessageClass::SuccessResponse, reader->GetTransactionId());
	resp.AddU32(stun::Attribute::Lifetime, 600u);
	resp.AppendMessageIntegrity(key);
	CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	CHECK(client.GetState() == TurnClient::State::Allocated);
}

TEST_CASE("Release sends a LIFETIME 0 Refresh and moves to Released without waiting for a reply")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	sent.clear();
	client.Release();
	CHECK(client.GetState() == TurnClient::State::Released);
	REQUIRE(sent.size() == 1);

	const auto reader = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(reader.has_value());
	CHECK(reader->GetMethod() == stun::Method::Refresh);
	const auto lifetime = reader->U32(stun::Attribute::Lifetime);
	REQUIRE(lifetime.has_value());
	CHECK(*lifetime == 0u);

	// No reply ever arrives - the release must not be waiting on one.
	client.Tick(30.0f);
	CHECK(sent.size() == 1);
	CHECK(client.GetState() == TurnClient::State::Released);
}

TEST_CASE("A truncated ChannelData datagram is dropped rather than read out of bounds")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	const std::vector<std::uint8_t> truncated{0x40, 0x01, 0x00, 0x10}; // claims 16 bytes of payload, holds none
	const auto delivery = client.OnDatagram(kServer, AsBytes(truncated));
	CHECK(delivery.consumed); // it came from the server, so it can only be relay traffic
	CHECK_FALSE(delivery.peer.has_value());
	CHECK(delivery.payload.empty());
}

TEST_CASE("A Data indication missing the DATA attribute is dropped rather than delivered with a guessed source")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	stun::MessageBuilder indication(stun::Method::Data, stun::MessageClass::Indication, stun::MakeTransactionId());
	indication.AddXorAddress(stun::Attribute::XorPeerAddress, kPeer); // no DATA attribute at all
	const auto delivery = client.OnDatagram(kServer, std::as_bytes(indication.Bytes()));

	CHECK(delivery.consumed);
	CHECK_FALSE(delivery.peer.has_value());
	CHECK(delivery.payload.empty());
}

TEST_CASE("A response carrying a transaction id nothing is waiting on changes nothing")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);
	const auto stateBefore = client.GetState();
	const auto relayedBefore = client.RelayedEndpoint();

	stun::MessageBuilder resp(stun::Method::Refresh, stun::MessageClass::SuccessResponse, stun::MakeTransactionId());
	resp.AddU32(stun::Attribute::Lifetime, 999u);
	resp.AppendMessageIntegrity(stun::crypto::LongTermKey(kUsername, kRealm, kPassword));
	const auto delivery = client.OnDatagram(kServer, std::as_bytes(resp.Bytes()));

	CHECK(delivery.consumed); // still consumed - it came from the server address
	CHECK(client.GetState() == stateBefore);
	CHECK(client.RelayedEndpoint() == relayedBefore);
}

TEST_CASE("A success response failing MESSAGE-INTEGRITY is ignored, not trusted")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);

	client.BeginAllocate();
	REQUIRE(sent.size() == 1);
	const auto probeId = stun::MessageReader::Parse(AsBytes(sent.back()))->GetTransactionId();
	{
		stun::MessageBuilder resp(stun::Method::Allocate, stun::MessageClass::ErrorResponse, probeId);
		resp.AddU32(stun::Attribute::ErrorCode, PackErrorCode(401));
		resp.AddText(stun::Attribute::Realm, kRealm);
		resp.AddText(stun::Attribute::Nonce, "nonce-1");
		CHECK(client.OnDatagram(kServer, std::as_bytes(resp.Bytes())).consumed);
	}
	REQUIRE(sent.size() == 2);
	const auto retryId = stun::MessageReader::Parse(AsBytes(sent.back()))->GetTransactionId();

	// A success response with the right transaction id but a bogus key - corruption, or a
	// forgery from anyone but the real relay.
	stun::MessageBuilder forged(stun::Method::Allocate, stun::MessageClass::SuccessResponse, retryId);
	forged.AddU32(stun::Attribute::Lifetime, 600u);
	forged.AddXorAddress(stun::Attribute::XorRelayedAddress, kRelayed);
	forged.AppendMessageIntegrity(stun::crypto::LongTermKey(kUsername, kRealm, "wrong-password"));
	CHECK(client.OnDatagram(kServer, std::as_bytes(forged.Bytes())).consumed);

	CHECK(client.GetState() == TurnClient::State::Allocating); // not Allocated - never trusted
	CHECK_FALSE(client.RelayedEndpoint().has_value());
}

TEST_CASE("A datagram from anyone but the configured TURN server is left alone")
{
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	const stun::Endpoint stranger = MakeEndpoint(0x0A0000FFu, 9999);
	const std::vector<std::uint8_t> junk{0x00, 0x01, 0x02, 0x03};
	const auto delivery = client.OnDatagram(stranger, AsBytes(junk));
	CHECK_FALSE(delivery.consumed);
	CHECK_FALSE(delivery.peer.has_value());
}

TEST_CASE("An unverified error response on the permission path does not replace the nonce")
{
	// An on-path attacker who sees the CreatePermission can race a forged 438 with
	// a nonce of their choosing. Post-auth every conformant server reply is
	// integrity-protected, so an unverified error is the forgery - adopting its
	// nonce wedges every later retry against the real server.
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	sent.clear();
	client.PermitPeer(kPeer);
	REQUIRE(sent.size() == 1);
	{
		const auto reader = stun::MessageReader::Parse(AsBytes(sent.back()));
		REQUIRE(reader.has_value());
		stun::MessageBuilder forged(stun::Method::CreatePermission, stun::MessageClass::ErrorResponse, reader->GetTransactionId());
		forged.AddU32(stun::Attribute::ErrorCode, PackErrorCode(438));
		forged.AddText(stun::Attribute::Nonce, "attacker-nonce"); // and no MESSAGE-INTEGRITY
		CHECK(client.OnDatagram(kServer, std::as_bytes(forged.Bytes())).consumed);
	}

	// Either a cooldown retry (pre-fix) or a retransmission (post-fix) follows; in
	// both worlds the next CreatePermission on the wire shows which nonce stuck.
	client.Tick(6.0f);
	REQUIRE(sent.size() == 2);
	const auto reader = stun::MessageReader::Parse(AsBytes(sent.back()));
	REQUIRE(reader.has_value());
	REQUIRE(reader->GetMethod() == stun::Method::CreatePermission);
	const auto nonce = reader->Text(stun::Attribute::Nonce);
	REQUIRE(nonce.has_value());
	CHECK(*nonce == "nonce-1"); // pre-fix this is "attacker-nonce"
}

TEST_CASE("Once every channel number is bound, an extra peer stays on Send indications instead of reusing a bound channel")
{
	// Channel numbers wrap 0x7FFF -> 0x4000. A wrapped bind used to land on a
	// number the FIRST-bound peer still holds, and OnDatagram hands ChannelData
	// to the first matching binding - misdelivering that peer's relayed traffic.
	//
	// Binding all 0x4000 numbers takes 0x4000 ticks, i.e. ~164 seconds of client
	// time, so allocation and permission refreshes necessarily fire part-way
	// through. This test therefore never asserts an exact send COUNT: it picks the
	// request it wants out of the log by method. Counting would pin the refresh
	// schedule, which is not what this test is about, and which is exactly how it
	// failed first time round.
	SentLog sent;
	TurnClient client = MakeClient(sent);
	DriveToAllocated(client, sent);

	const auto lastOfMethod = [](const SentLog& log, stun::Method method) -> std::vector<std::uint8_t>
	{
		for (auto it = log.rbegin(); it != log.rend(); ++it)
		{
			const auto reader = stun::MessageReader::Parse(AsBytes(*it));
			if (reader.has_value() && reader->GetMethod() == method)
			{
				return *it;
			}
		}
		return {};
	};

	// Bind all 0x4000 channel numbers (0x4000..0x7FFF), one per peer.
	for (std::uint32_t i = 0; i < 0x4000u; ++i)
	{
		const stun::Endpoint peer = MakeEndpoint(0x0A000000u + i + 1u, 40000);
		client.PermitPeer(peer);
		const std::vector<std::uint8_t> permission = lastOfMethod(sent, stun::Method::CreatePermission);
		REQUIRE_FALSE(permission.empty());
		RespondSuccess(client, stun::Method::CreatePermission, permission);
		client.Tick(0.01f);
		const std::vector<std::uint8_t> bind = lastOfMethod(sent, stun::Method::ChannelBind);
		REQUIRE_FALSE(bind.empty());
		RespondSuccess(client, stun::Method::ChannelBind, bind);
		sent.clear();
	}

	// Peer number 0x4001: the counter has wrapped, and no number is free.
	const stun::Endpoint extra = MakeEndpoint(0x0A000000u + 0x4001u, 40000);
	client.PermitPeer(extra);
	const std::vector<std::uint8_t> extraPermission = lastOfMethod(sent, stun::Method::CreatePermission);
	REQUIRE_FALSE(extraPermission.empty());
	RespondSuccess(client, stun::Method::CreatePermission, extraPermission);
	client.Tick(0.01f);

	// Pre-fix this tick produced a ChannelBind reusing 0x4000; post-fix there is
	// no free number, so the peer must stay on Send indications.
	CHECK(lastOfMethod(sent, stun::Method::ChannelBind).empty());

	sent.clear();
	const std::vector<std::byte> payload{std::byte{0x01}};
	client.SendToPeer(extra, payload);
	REQUIRE_FALSE(sent.empty());
	// A Send indication, never ChannelData on a channel another peer still holds.
	for (const auto& raw: sent)
	{
		CHECK_FALSE(stun::LooksLikeChannelData(AsBytes(raw)));
	}
}
