// The whole of aether-rendezvous except the socket loop: grammar, room/peer
// tables, abuse bounds, and the per-datagram policy, all as free functions over
// plain data so a test can drive them with no socket, no ENet and no clock.
// main.cpp is a thin shell that pumps datagrams through ProcessDatagram and
// sends whatever comes back. Nothing here links anything from the engine, so
// the tool stays a standalone binary (see CMakeLists.txt).

#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rendezvous
{
	// Wire protocol, one datagram per line, byte for byte the same shape in both
	// directions (verbatim, for the client side to match):
	//   client -> server: "AECR2 <ROOM> <TOKEN> <BLOB>\n"
	//   server -> client: "AECR2 <ROOM> <TOKEN> <BODY>\n"
	// <ROOM> is the six-character room code. <TOKEN> is "-" on a client's first
	// contact (it has not been issued one yet) and the 16-uppercase-hex token the
	// server previously sent to that exact source address in every later line;
	// the server always fills it with the RECIPIENT's own token. <BLOB> is
	// whatever aether::net::EncodeCandidates() produced - it contains spaces, so
	// it is always the remainder of the line. <BODY> is either one other verified
	// peer's blob or "-" (a handshake-only reply carrying nothing but the token).
	// A trailing CRLF is tolerated as well as a bare LF; anything else is refused.
	constexpr std::string_view kMagic = "AECR2 ";

	// The AECR1 protocol had no token round trip, which made the server both a
	// reflection cannon and trivially poisonable (any spoofed-source datagram
	// overwrote a peer's blob). AECR2 breaks the wire to close that: an AECR1
	// line does not parse here and gets no reply, so mixed-version pairs fail
	// fast instead of half-working.
	constexpr std::size_t kTokenHexLength = 16;

	constexpr std::uint16_t kDefaultPort = 24701;

	// A blob only ever holds up to 8 "dotted-quad:port" entries plus a version
	// tag (see Signaling.hpp), so a real message never comes close to this. A
	// datagram over the cap is dropped outright rather than parsed - an attacker
	// gets nothing for padding a packet.
	constexpr std::size_t kMaxDatagramBytes = 1200;

	// A room code is fixed at six characters after normalization; a raw token far
	// longer than that cannot possibly normalize to one, so it is rejected before
	// the per-character work below runs.
	constexpr std::size_t kMaxRawRoomTokenBytes = 64;

	constexpr std::size_t kRoomCodeLength = 6;
	constexpr std::string_view kRoomCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32, minus I/L/O/U

	// Bounds on live state so a burst of forged senders cannot grow memory without
	// limit: this server holds nothing but small in-memory maps, so its whole safety
	// story is capping and expiring them.
	constexpr std::size_t kMaxRooms = 4096;
	constexpr std::size_t kMaxPeersPerRoom = 16;
	constexpr std::size_t kMaxTrackedSources = 4096;

	// A peer that completed the token round trip is a proven address and lives as
	// long as a punch attempt can plausibly take. A peer that only sent one
	// datagram is unproven - most such entries are spoof-flood filler - so they
	// churn fast instead of squatting a room slot for the full two minutes.
	constexpr std::uint32_t kIdleTimeoutMs = 120'000;
	constexpr std::uint32_t kUnverifiedTimeoutMs = 10'000;
	constexpr std::uint32_t kSweepIntervalMs = 5'000;
	constexpr std::uint32_t kStatsIntervalMs = 30'000;
	constexpr std::uint32_t kWaitTimeoutMs = 1'000;

	// Per-source-host (IP, not port: a spoofer picks the port freely) bounds on
	// what one datagram sender can make this server emit towards others, in a
	// fixed one-second window. The token-reply cap is what keeps spoofed-source
	// reflection inert: even a fully spoofed flood buys at most eight ~34-byte
	// token lines per claimed victim per second, carrying no room data. The byte
	// cap bounds a proven-but-hostile member's fan-out to a full room. Honest
	// traffic sits orders of magnitude below both (a two-peer room costs a few
	// hundred bytes per publish, one token per session).
	constexpr std::uint32_t kSourceWindowMs = 1'000;
	constexpr std::uint32_t kSourceIdleTimeoutMs = 30'000;
	constexpr std::size_t kMaxTokenRepliesPerSourcePerWindow = 8;
	constexpr std::size_t kMaxOutboundBytesPerSourcePerWindow = 32'768;

	// Standalone address value so this header needs no ENet type: main.cpp
	// converts at the socket boundary and the policy stays testable bare.
	struct Address
	{
		std::uint32_t host = 0;
		std::uint16_t port = 0;
		bool operator==(const Address&) const = default;
	};

	// Reimplements aether::net::NormalizeRoomCode's documented behavior
	// standalone: this tool deliberately links nothing from the engine (see
	// CMakeLists.txt), so it carries its own six lines rather than a dependency.
	// Case-insensitive; maps the confusable set (I/l -> 1, O -> 0) the alphabet
	// excludes; strips spaces and dashes; returns nullopt for anything that is
	// not a room code once normalized.
	std::optional<std::string> NormalizeRoomCode(std::string_view text)
	{
		if (text.size() > kMaxRawRoomTokenBytes)
		{
			return std::nullopt;
		}

		std::string result;
		result.reserve(text.size());
		for (const char c : text)
		{
			if (c == ' ' || c == '-')
			{
				continue;
			}
			char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			if (upper == 'I' || upper == 'L')
			{
				upper = '1';
			}
			else if (upper == 'O')
			{
				upper = '0';
			}
			result.push_back(upper);
		}

		if (result.size() != kRoomCodeLength)
		{
			return std::nullopt;
		}
		for (const char c : result)
		{
			if (kRoomCodeAlphabet.find(c) == std::string_view::npos)
			{
				return std::nullopt;
			}
		}
		return result;
	}

	struct ParsedMessage
	{
		std::string_view roomToken;
		std::optional<std::uint64_t> token; // nullopt = the sender has not been issued one yet
		std::string_view blob;
	};

	// Refuses anything it does not fully understand rather than salvaging part of
	// it, matching the spirit of DecodeCandidates: a half-read line must never be
	// forwarded as if it were a real candidate blob. In particular an AECR1 line
	// (the pre-token protocol) fails the magic check and is dropped unread.
	std::optional<ParsedMessage> ParseLine(std::string_view line)
	{
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		{
			line.remove_suffix(1);
		}
		if (line.size() <= kMagic.size() || line.substr(0, kMagic.size()) != kMagic)
		{
			return std::nullopt;
		}
		// One datagram is one line; an embedded terminator means the sender is
		// smuggling a second line, which this protocol never sends.
		if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos)
		{
			return std::nullopt;
		}

		const std::string_view rest = line.substr(kMagic.size());
		const std::size_t roomEnd = rest.find(' ');
		if (roomEnd == std::string_view::npos)
		{
			return std::nullopt;
		}
		const std::string_view roomToken = rest.substr(0, roomEnd);

		const std::string_view afterRoom = rest.substr(roomEnd + 1);
		const std::size_t tokenEnd = afterRoom.find(' ');
		if (tokenEnd == std::string_view::npos)
		{
			return std::nullopt;
		}
		const std::string_view tokenField = afterRoom.substr(0, tokenEnd);
		const std::string_view blob = afterRoom.substr(tokenEnd + 1);

		std::optional<std::uint64_t> tokenValue;
		if (tokenField != "-")
		{
			if (tokenField.size() != kTokenHexLength)
			{
				return std::nullopt;
			}
			std::uint64_t value = 0;
			const char* begin = tokenField.data();
			const char* end = begin + tokenField.size();
			const auto result = std::from_chars(begin, end, value, 16);
			if (result.ec != std::errc{} || result.ptr != end)
			{
				return std::nullopt;
			}
			tokenValue = value;
		}

		if (roomToken.empty() || blob.empty())
		{
			return std::nullopt;
		}
		return ParsedMessage{roomToken, tokenValue, blob};
	}

	std::string EncodeToken(std::uint64_t token)
	{
		constexpr char kHex[] = "0123456789ABCDEF";
		std::string out(kTokenHexLength, '0');
		for (int i = static_cast<int>(kTokenHexLength) - 1; i >= 0; --i)
		{
			out[static_cast<std::size_t>(i)] = kHex[token & 0xF];
			token >>= 4;
		}
		return out;
	}

	// One line of the protocol, both directions. The token is always the
	// RECIPIENT's own, so a client can recover it from any datagram it receives.
	std::string EncodeLine(std::string_view roomCode, std::uint64_t recipientToken, std::string_view body)
	{
		std::string line;
		line.reserve(kMagic.size() + roomCode.size() + 1 + kTokenHexLength + 1 + body.size() + 1);
		line.append(kMagic);
		line.append(roomCode);
		line.push_back(' ');
		line.append(EncodeToken(recipientToken));
		line.push_back(' ');
		line.append(body);
		line.push_back('\n');
		return line;
	}

	// Tokens come straight from the platform CSPRNG rather than a seeded PRNG: a
	// seeded generator's future outputs can in principle be reconstructed from
	// enough of its past ones, and a minter can watch its own tokens for free.
	// Mints are rare (once per peer per room), so the per-call cost is nothing.
	// Zero is never minted, keeping "no token" unambiguous if it ever needs a
	// sentinel.
	std::uint64_t MintToken()
	{
		std::random_device device;
		const std::uint64_t hi = device();
		const std::uint64_t lo = device();
		const std::uint64_t token = (hi << 32) | (lo & 0xFFFF'FFFFull);
		return token != 0 ? token : 1;
	}

	struct Peer
	{
		Address address{};
		std::string blob;
		std::uint32_t lastSeenMs = 0;
		std::uint64_t token = 0; // the capability token this address must present to update state
		bool verified = false; // the address has completed one token round trip
	};

	struct Room
	{
		std::vector<Peer> peers;
	};

	// One sender host's outbound budget. Keyed by IP alone because a spoofer
	// chooses the claimed port freely; a NAT household sharing one IP still fits
	// comfortably inside the caps. Fixed one-second windows: an abuser can double-
	// burst across a boundary, which is irrelevant at these sizes.
	struct SourceState
	{
		std::uint32_t windowStartMs = 0;
		std::uint32_t lastSeenMs = 0;
		std::size_t bytesThisWindow = 0;
		std::size_t tokenRepliesThisWindow = 0;
	};

	struct ServerState
	{
		std::unordered_map<std::string, Room> rooms;
		std::unordered_map<std::uint32_t, SourceState> sources;
	};

	// One datagram the policy wants on the wire. main.cpp only forwards these to
	// the socket; it never decides anything.
	struct OutboundDatagram
	{
		Address to;
		std::string line;
	};

	void RollWindow(SourceState& source, std::uint32_t now)
	{
		if (now - source.windowStartMs >= kSourceWindowMs)
		{
			source.windowStartMs = now;
			source.bytesThisWindow = 0;
			source.tokenRepliesThisWindow = 0;
		}
	}

	bool TrySpendBytes(SourceState& source, std::size_t bytes)
	{
		if (source.bytesThisWindow + bytes > kMaxOutboundBytesPerSourcePerWindow)
		{
			return false;
		}
		source.bytesThisWindow += bytes;
		return true;
	}

	SourceState& FindOrAddSource(ServerState& state, std::uint32_t host, std::uint32_t now)
	{
		if (const auto it = state.sources.find(host); it != state.sources.end())
		{
			it->second.lastSeenMs = now;
			return it->second;
		}
		if (state.sources.size() >= kMaxTrackedSources)
		{
			// Steal the source with the oldest window rather than refusing: window
			// starts track last activity (RollWindow refreshes on arrival), so the
			// victim is an idle entry and a flood cannot grow the table past the
			// cap. Scanning only happens on this rare at-cap insert.
			auto victim = state.sources.begin();
			for (auto it = state.sources.begin(); it != state.sources.end(); ++it)
			{
				if (now - it->second.windowStartMs > now - victim->second.windowStartMs)
				{
					victim = it; // unsigned-difference compare across the u32 wrap
				}
			}
			state.sources.erase(victim);
		}
		SourceState& source = state.sources[host];
		source.windowStartMs = now;
		source.lastSeenMs = now;
		return source;
	}

	// Rooms live only as long as they hold at least one peer, so "room expires
	// after 120s idle" falls out of "every peer in it expired": there is no
	// separate room-level timer to keep in sync with the peer one.
	// At the cap this EVICTS instead of refusing: a flood of distinct codes must
	// degrade stale state, not lock every new session out for two minutes. A room
	// with no verified peer is flood filler by definition and goes first; beyond
	// those, the room whose newest activity is oldest goes - so a flood mostly
	// evicts its own garbage.
	Room& FindOrCreateRoom(ServerState& state, const std::string& code, std::uint32_t now)
	{
		if (const auto it = state.rooms.find(code); it != state.rooms.end())
		{
			return it->second;
		}
		if (state.rooms.size() >= kMaxRooms)
		{
			auto victim = state.rooms.end();
			std::uint32_t victimFreshness = 0;
			bool victimIsFiller = false;
			for (auto it = state.rooms.begin(); it != state.rooms.end(); ++it)
			{
				bool filler = true;
				std::uint32_t freshness = 0;
				for (const Peer& peer : it->second.peers)
				{
					if (peer.verified)
					{
						filler = false;
					}
					if (now - peer.lastSeenMs < now - freshness)
					{
						freshness = peer.lastSeenMs;
					}
				}
				const bool better =
					(victim == state.rooms.end()) || (filler && !victimIsFiller) ||
					(filler == victimIsFiller && now - freshness > now - victimFreshness);
				if (better)
				{
					victim = it;
					victimIsFiller = filler;
					victimFreshness = freshness;
				}
			}
			state.rooms.erase(victim);
		}
		return state.rooms.emplace(code, Room{}).first->second;
	}

	Peer* FindOrAddPeer(Room& room, const Address& address, std::uint32_t now)
	{
		for (Peer& peer : room.peers)
		{
			if (peer.address == address)
			{
				return &peer;
			}
		}
		if (room.peers.size() >= kMaxPeersPerRoom)
		{
			// A full room must not be wedged by unverified filler: displace the
			// oldest unproven entry so a real peer still gets a slot. Verified
			// peers are never displaced - a proven address losing its slot to a
			// flood would reset its token and its blob mid-punch.
			Peer* oldestUnverified = nullptr;
			for (Peer& peer : room.peers)
			{
				if (!peer.verified && (oldestUnverified == nullptr || now - peer.lastSeenMs > now - oldestUnverified->lastSeenMs))
				{
					oldestUnverified = &peer;
				}
			}
			if (oldestUnverified == nullptr)
			{
				return nullptr; // genuinely full of proven peers
			}
			*oldestUnverified = Peer{address, {}, now, MintToken(), false};
			return oldestUnverified;
		}
		room.peers.push_back(Peer{address, {}, now, MintToken(), false});
		return &room.peers.back();
	}

	// On a datagram: normalize the room code (drop if it fails), then either
	// complete a token handshake or service a proven peer.
	//
	// Unproven sender (no token, wrong token, or a brand-new entry): reply with
	// exactly one datagram - that peer's own token - and change nothing else. The
	// blob is ignored, the stored blob is untouched and lastSeen is not
	// refreshed, so a spoofed-source datagram can neither poison a victim's blob
	// nor keep a poisoned entry alive. One small reply per inbound also caps
	// reflection at ~1:1 with inert content, and the per-source token-reply
	// counter caps even that when the claimed source is being flooded.
	//
	// Proven sender (presents the token this address was issued for this room):
	// store the blob, refresh liveness, forward the blob to every OTHER verified
	// peer, and answer the sender with those peers' blobs. Unverified peers are
	// skipped in both directions - an address that has not proven it can receive
	// must never be sent room data, or a spoofer could aim other players' blobs
	// at a victim just by claiming its address once.
	void ProcessDatagram(ServerState& state, const Address& from, std::string_view raw, std::uint32_t now, std::vector<OutboundDatagram>& out)
	{
		if (raw.size() > kMaxDatagramBytes)
		{
			return;
		}
		const std::optional<ParsedMessage> parsed = ParseLine(raw);
		if (!parsed.has_value())
		{
			return;
		}
		const std::optional<std::string> roomCode = NormalizeRoomCode(parsed->roomToken);
		if (!roomCode.has_value())
		{
			return;
		}

		SourceState& source = FindOrAddSource(state, from.host, now);
		RollWindow(source, now);

		Room& room = FindOrCreateRoom(state, *roomCode, now);
		Peer* sender = FindOrAddPeer(room, from, now);
		if (sender == nullptr)
		{
			return; // Room already holds as many proven peers as a punch session could ever use.
		}

		if (parsed->token.has_value() && *parsed->token == sender->token)
		{
			sender->verified = true;
			sender->blob.assign(parsed->blob);
			sender->lastSeenMs = now;

			for (const Peer& peer : room.peers)
			{
				if (peer.address == from || !peer.verified)
				{
					continue;
				}
				std::string line = EncodeLine(*roomCode, peer.token, sender->blob);
				if (!TrySpendBytes(source, line.size()))
				{
					continue; // this source spent its window; honest peers retry next window
				}
				out.push_back(OutboundDatagram{peer.address, std::move(line)});
			}
			for (const Peer& peer : room.peers)
			{
				if (peer.address == from || !peer.verified)
				{
					continue;
				}
				std::string line = EncodeLine(*roomCode, sender->token, peer.blob);
				if (!TrySpendBytes(source, line.size()))
				{
					continue;
				}
				out.push_back(OutboundDatagram{from, std::move(line)});
			}
			return;
		}

		// First contact, a lost token, or a guess: hand out (only) the token. The
		// reply is identical whether the room is empty or full, so probing codes
		// without completing a round trip learns nothing about occupancy.
		if (source.tokenRepliesThisWindow >= kMaxTokenRepliesPerSourcePerWindow)
		{
			return;
		}
		std::string line = EncodeLine(*roomCode, sender->token, "-");
		if (!TrySpendBytes(source, line.size()))
		{
			return;
		}
		++source.tokenRepliesThisWindow;
		out.push_back(OutboundDatagram{from, std::move(line)});
	}

	void SweepExpired(ServerState& state, std::uint32_t now)
	{
		for (auto it = state.rooms.begin(); it != state.rooms.end();)
		{
			// Each peer expires on its own clock: 120s once proven, a fast 10s
			// while unproven so spoof-flood filler churns instead of squatting.
			std::erase_if(it->second.peers, [&](const Peer& peer)
			{
				const std::uint32_t peerTimeout = peer.verified ? kIdleTimeoutMs : kUnverifiedTimeoutMs;
				return now - peer.lastSeenMs >= peerTimeout;
			});
			if (it->second.peers.empty())
			{
				it = state.rooms.erase(it);
			}
			else
			{
				++it;
			}
		}
		for (auto it = state.sources.begin(); it != state.sources.end();)
		{
			it = (now - it->second.lastSeenMs >= kSourceIdleTimeoutMs) ? state.sources.erase(it) : std::next(it);
		}
	}
} // namespace rendezvous
