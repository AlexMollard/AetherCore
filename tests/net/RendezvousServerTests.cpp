#include <doctest/doctest.h>
#include <ostream>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The rendezvous server's policy, pinned without a socket: every case drives
// rendezvous::ProcessDatagram and inspects only the datagrams it emits. The
// tool's main.cpp is a thin socket shell over exactly this code, so these
// checks are the wire behaviour a real client sees.
#include "../../tools/rendezvous/RendezvousServerLogic.hpp"

namespace
{
	rendezvous::Address At(std::uint32_t host, std::uint16_t port)
	{
		return rendezvous::Address{host, port};
	}

	// Test-side parser for server lines, deliberately hand-rolled rather than
	// reusing the server's own code, so a grammar bug cannot hide behind itself.
	struct ServerLine
	{
		std::string room;
		std::uint64_t token = 0;
		std::string body;
	};

	std::optional<ServerLine> ParseServerLine(std::string_view line)
	{
		if (!line.empty() && line.back() == '\n')
		{
			line.remove_suffix(1);
		}
		if (line.substr(0, 6) != "AECR2 ")
		{
			return std::nullopt;
		}
		const std::string_view afterMagic = line.substr(6);
		const std::size_t roomEnd = afterMagic.find(' ');
		if (roomEnd == std::string_view::npos || roomEnd == 0)
		{
			return std::nullopt;
		}
		const std::string_view afterRoom = afterMagic.substr(roomEnd + 1);
		// The token is exactly 16 hex digits followed by one space; the body is
		// everything after that and may itself contain spaces (blobs do).
		if (afterRoom.size() < 17 || afterRoom[16] != ' ')
		{
			return std::nullopt;
		}
		const std::string_view tokenField = afterRoom.substr(0, 16);

		ServerLine out;
		out.room = std::string(afterMagic.substr(0, roomEnd));
		for (const char c : tokenField)
		{
			const int digit = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
			if (digit < 0)
			{
				return std::nullopt;
			}
			out.token = (out.token << 4) | static_cast<std::uint64_t>(digit);
		}
		out.body = std::string(afterRoom.substr(17));
		if (out.body.empty())
		{
			return std::nullopt;
		}
		return out;
	}

	std::string FirstContact(std::string_view room, std::string_view blob)
	{
		std::string line = "AECR2 ";
		line += room;
		line += " - ";
		line += blob;
		line += '\n';
		return line;
	}

	std::uint64_t Handshake(rendezvous::ServerState& state, std::string_view room, rendezvous::Address peer, std::uint32_t now)
	{
		std::vector<rendezvous::OutboundDatagram> out;
		rendezvous::ProcessDatagram(state, peer, FirstContact(room, "v1 10.0.0.1:1000"), now, out);
		REQUIRE(out.size() == 1);
		REQUIRE(out[0].to == peer);
		const auto line = ParseServerLine(out[0].line);
		REQUIRE(line.has_value());
		CHECK(line->body == "-");
		return line->token;
	}

	void Publish(rendezvous::ServerState& state, std::string_view room, rendezvous::Address peer, std::uint64_t token, std::string_view blob, std::uint32_t now, std::vector<rendezvous::OutboundDatagram>& out)
	{
		rendezvous::ProcessDatagram(state, peer, rendezvous::EncodeLine(room, token, blob), now, out);
	}
} // namespace

TEST_CASE("ParseLine pins the AECR2 grammar byte for byte")
{
	const auto parsed = rendezvous::ParseLine("AECR2 ABCDEF - v1 192.168.1.40:24710 203.0.113.9:41234\n");
	REQUIRE(parsed.has_value());
	CHECK(parsed->roomToken == "ABCDEF");
	CHECK_FALSE(parsed->token.has_value());
	CHECK(parsed->blob == "v1 192.168.1.40:24710 203.0.113.9:41234");

	const std::uint64_t token = 0x1234567890ABCDEF;
	const auto withToken = rendezvous::ParseLine("AECR2 ABCDEF " + rendezvous::EncodeToken(token) + " v1 x\n");
	REQUIRE(withToken.has_value());
	REQUIRE(withToken->token.has_value());
	CHECK(*withToken->token == token);

	// The client may send lowercase hex; the server always answers uppercase.
	SUBCASE("case-insensitive hex token") { CHECK(rendezvous::ParseLine("AECR2 ABCDEF 1234abcd90abcdef v1 x\n").has_value()); }
	SUBCASE("AECR1 is the pre-token protocol and must not parse") { CHECK_FALSE(rendezvous::ParseLine("AECR1 ABCDEF - v1 x\n").has_value()); }
	SUBCASE("missing token field") { CHECK_FALSE(rendezvous::ParseLine("AECR2 ABCDEF v1 x\n").has_value()); }
	SUBCASE("no blob") { CHECK_FALSE(rendezvous::ParseLine("AECR2 ABCDEF -\n").has_value()); }
	SUBCASE("short token") { CHECK_FALSE(rendezvous::ParseLine("AECR2 ABCDEF 123 v1 x\n").has_value()); }
	SUBCASE("long token") { CHECK_FALSE(rendezvous::ParseLine("AECR2 ABCDEF 1234567890abcdef0 v1 x\n").has_value()); }
	SUBCASE("non-hex token") { CHECK_FALSE(rendezvous::ParseLine("AECR2 ABCDEF 1234567890abcdeg v1 x\n").has_value()); }
	SUBCASE("embedded CR smuggles a second line") { CHECK_FALSE(rendezvous::ParseLine("AECR2 ABCDEF - v1 x\rAECR2 ABCDEF - y\n").has_value()); }
	SUBCASE("bare junk") { CHECK_FALSE(rendezvous::ParseLine("\n").has_value()); }
}

TEST_CASE("A first contact is answered with its token and nothing else, even in a full room")
{
	rendezvous::ServerState state;
	// "PACKED", not "FULL01": U is outside the Crockford alphabet entirely, so
	// NormalizeRoomCode refuses it and the room is never created - the test would
	// pass vacuously against an empty server. Every literal here is already in
	// normal form (no I/L/O to fold), so what the test types is what the server keys.
	constexpr std::string_view room = "PACKED";
	for (std::uint32_t host = 2; host <= 16; ++host)
	{
		const auto token = Handshake(state, room, At(host, 1), 1000);
		std::vector<rendezvous::OutboundDatagram> out;
		Publish(state, room, At(host, 1), token, "v1 10.0.0." + std::to_string(host) + ":2000", 1000, out);
	}

	// An unauthenticated datagram - the reflection case - must buy exactly one
	// small reply addressed to the claimed source, no matter how many members
	// the room has or what blobs they published.
	std::vector<rendezvous::OutboundDatagram> out;
	const rendezvous::Address attacker = At(99, 1);
	rendezvous::ProcessDatagram(state, attacker, FirstContact(room, "v1 6.6.6.6:66"), 1000, out);
	REQUIRE(out.size() == 1);
	CHECK(out[0].to == attacker);
	const auto line = ParseServerLine(out[0].line);
	REQUIRE(line.has_value());
	CHECK(line->body == "-");
	CHECK(out[0].line.find("10.0.0.") == std::string::npos);
}

TEST_CASE("Room data never reaches a peer that has not completed the handshake")
{
	rendezvous::ServerState state;
	constexpr std::string_view room = "LEAK01";
	const auto tokenA = Handshake(state, room, At(1, 1), 1000);
	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, room, At(1, 1), tokenA, "v1 10.0.0.1:1111", 1000, out);
	REQUIRE(out.empty()); // a lone verified peer has nobody to talk to yet

	// A third party claiming an address it may not own gets the token handshake
	// and nothing else - the room's real candidates must not ride along, and the
	// reply must not reveal whether the room is occupied at all.
	rendezvous::ProcessDatagram(state, At(2, 1), FirstContact(room, "v1 6.6.6.6:66"), 1000, out);
	REQUIRE(out.size() == 1);
	const auto line = ParseServerLine(out[0].line);
	REQUIRE(line.has_value());
	CHECK(line->body == "-");
	CHECK(out[0].line.find("10.0.0.1:1111") == std::string::npos);

	out.clear();
	rendezvous::ProcessDatagram(state, At(3, 1), FirstContact("EMPTY9", "v1 6.6.6.6:66"), 1000, out);
	REQUIRE(out.size() == 1);
	const auto empty = ParseServerLine(out[0].line);
	REQUIRE(empty.has_value());
	CHECK(empty->body == "-"); // occupied and empty rooms answer identically pre-auth
}

TEST_CASE("A spoofed datagram can neither replace a verified peer's blob nor keep it alive")
{
	rendezvous::ServerState state;
	constexpr std::string_view room = "POISON";
	constexpr std::uint32_t t0 = 1000;
	const rendezvous::Address victim = At(1, 1);
	const rendezvous::Address other = At(2, 1);
	const auto tokenVictim = Handshake(state, room, victim, t0);
	const auto tokenOther = Handshake(state, room, other, t0);
	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, room, victim, tokenVictim, "v1 10.0.0.1:1111", t0, out);
	Publish(state, room, other, tokenOther, "v1 10.0.0.2:2222", t0, out);
	out.clear();

	// Finding 2's repro: datagrams from the victim's address without its token.
	SUBCASE("tokenless overwrite") { rendezvous::ProcessDatagram(state, victim, FirstContact(room, "v1 9.9.9.9:9999"), t0, out); }
	SUBCASE("wrong-token overwrite")
	{
		rendezvous::ProcessDatagram(state, victim, rendezvous::EncodeLine(room, 0xDEADBEEFDEADBEEFull, "v1 9.9.9.9:9999"), t0, out);
	}
	REQUIRE(out.size() == 1); // only the token re-issue
	CHECK(out[0].to == victim);
	const auto line = ParseServerLine(out[0].line);
	REQUIRE(line.has_value());
	CHECK(line->token == tokenVictim); // its own token, not an acceptance
	out.clear();

	// The stored blob survived untouched...
	Publish(state, room, other, tokenOther, "v1 10.0.0.2:2223", t0 + 119'999, out);
	bool sawIntactBlob = false;
	for (const auto& datagram : out)
	{
		if (datagram.to == other && datagram.line.find("10.0.0.1:1111") != std::string::npos)
		{
			sawIntactBlob = true;
		}
		CHECK(datagram.line.find("9.9.9.9") == std::string::npos);
	}
	CHECK(sawIntactBlob);

	// ...and the spoof did not refresh liveness: the victim expires 120s after
	// its last REAL publish, not after the spoof.
	rendezvous::SweepExpired(state, t0 + 120'000);
	out.clear();
	Publish(state, room, other, tokenOther, "v1 10.0.0.2:2224", t0 + 120'001, out);
	for (const auto& datagram : out)
	{
		CHECK(datagram.to != victim); // gone; nothing is forwarded at a dead entry
		if (datagram.to == other)
		{
			CHECK(datagram.line.find("10.0.0.1:1111") == std::string::npos);
		}
	}
}

TEST_CASE("Completing the handshake unlocks the two-way exchange")
{
	rendezvous::ServerState state;
	constexpr std::string_view room = "PA9R01"; // already normal form; "PAIR01" folds its I to 1
	const rendezvous::Address a = At(1, 1);
	const rendezvous::Address b = At(2, 1);
	const auto tokenA = Handshake(state, room, a, 1000);
	const auto tokenB = Handshake(state, room, b, 1000);

	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, room, a, tokenA, "v1 10.0.0.1:1111", 1000, out);
	REQUIRE(out.empty()); // still alone: A's publish is stored but nobody to tell

	Publish(state, room, b, tokenB, "v1 10.0.0.2:2222", 1000, out);
	REQUIRE(out.size() == 2);
	for (const auto& datagram : out)
	{
		const auto line = ParseServerLine(datagram.line);
		REQUIRE(line.has_value());
		CHECK(line->room == "PA9R01");
		if (datagram.to == a)
		{
			CHECK(line->body == "v1 10.0.0.2:2222"); // B's blob forwarded to A
			CHECK(line->token == tokenA); // every line carries the RECIPIENT's token
		}
		else
		{
			REQUIRE(datagram.to == b);
			CHECK(line->body == "v1 10.0.0.1:1111"); // A's blob answered to B
			CHECK(line->token == tokenB);
		}
	}
}

TEST_CASE("A token is bound to one room and one address")
{
	rendezvous::ServerState state;
	const auto tokenA = Handshake(state, "ROOMA1", At(1, 1), 1000);
	std::vector<rendezvous::OutboundDatagram> out;

	// Accepted in its own room: a lone verified peer's publish says nothing.
	Publish(state, "ROOMA1", At(1, 1), tokenA, "v1 10.0.0.1:1111", 1000, out);
	CHECK(out.empty());

	// The same token presented for a different room is just a first contact there.
	rendezvous::ProcessDatagram(state, At(1, 1), rendezvous::EncodeLine("ROOMB1", tokenA, "v1 9.9.9.9:9999"), 1000, out);
	REQUIRE(out.size() == 1);
	const auto crossRoom = ParseServerLine(out[0].line);
	REQUIRE(crossRoom.has_value());
	CHECK(crossRoom->body == "-");
	out.clear();

	// And presenting A's token from a different address buys nothing either.
	rendezvous::ProcessDatagram(state, At(2, 1), rendezvous::EncodeLine("ROOMA1", tokenA, "v1 9.9.9.9:9999"), 1000, out);
	REQUIRE(out.size() == 1);
	const auto crossAddress = ParseServerLine(out[0].line);
	REQUIRE(crossAddress.has_value());
	CHECK(crossAddress->body == "-");
}

TEST_CASE("Unverified filler expires in seconds, not minutes")
{
	rendezvous::ServerState state;
	constexpr std::string_view room = "TTL001";
	const auto tokenA = Handshake(state, room, At(1, 1), 0);
	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, room, At(1, 1), tokenA, "v1 10.0.0.1:1111", 0, out); // A verified at t=0

	// A peer that only ever sent one datagram (handshake, no token back) is
	// filler: it must be gone at the 10s mark...
	const auto tokenFiller = Handshake(state, room, At(2, 1), 0);
	rendezvous::SweepExpired(state, 10'001);
	Publish(state, room, At(2, 1), tokenFiller, "v1 10.0.0.2:2222", 10'001, out);
	REQUIRE(out.size() == 1);
	CHECK(ParseServerLine(out[0].line)->body == "-"); // rejected: entry expired, token gone with it
	out.clear();

	// ...while the verified peer is still admitted at the same instant.
	Publish(state, room, At(1, 1), tokenA, "v1 10.0.0.1:1112", 10'001, out);
	CHECK(out.empty());
}

TEST_CASE("At the room cap, flood filler is evicted before a live room")
{
	rendezvous::ServerState state;
	for (std::uint32_t i = 0; i < rendezvous::kMaxRooms; ++i)
	{
		Handshake(state, "F" + rendezvous::EncodeToken(i).substr(11), At(i + 1, 1), 100);
	}
	REQUIRE(state.rooms.size() == rendezvous::kMaxRooms);

	// The one room with a proven peer is the session that must survive.
	const auto tokenReal = Handshake(state, "REAL01", At(5000, 1), 200);
	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, "REAL01", At(5000, 1), tokenReal, "v1 10.0.0.1:1111", 200, out);
	REQUIRE(out.empty());
	CHECK(state.rooms.size() == rendezvous::kMaxRooms); // admitting it evicted a filler

	// A new room still forms at the cap...
	Handshake(state, "NEWCDE", At(6000, 1), 300);
	CHECK(state.rooms.size() == rendezvous::kMaxRooms);

	// ...and the live room's peer is still verified: its token still works.
	out.clear();
	Publish(state, "REAL01", At(5000, 1), tokenReal, "v1 10.0.0.1:1112", 300, out);
	CHECK(out.empty()); // an evicted peer would have been re-minted and answered with "-"
}

TEST_CASE("When every room is live, the stalest one is evicted")
{
	rendezvous::ServerState state;
	for (std::uint32_t i = 0; i + 1 < rendezvous::kMaxRooms; ++i)
	{
		const std::string code = "K" + rendezvous::EncodeToken(i).substr(11);
		const auto token = Handshake(state, code, At(i + 1, 1), 1000);
		std::vector<rendezvous::OutboundDatagram> out;
		Publish(state, code, At(i + 1, 1), token, "v1 10.0.0.1:1111", 1000, out);
	}
	const auto tokenStale = Handshake(state, "STALE1", At(4999, 1), 500);
	std::vector<rendezvous::OutboundDatagram> staleOut;
	Publish(state, "STALE1", At(4999, 1), tokenStale, "v1 10.0.0.1:1111", 500, staleOut);
	CHECK(state.rooms.size() == rendezvous::kMaxRooms);

	Handshake(state, "NEWCDE", At(6000, 1), 2000);

	staleOut.clear();
	Publish(state, "STALE1", At(4999, 1), tokenStale, "v1 10.0.0.1:1112", 2000, staleOut);
	REQUIRE(staleOut.size() == 1);
	CHECK(ParseServerLine(staleOut[0].line)->body == "-"); // evicted: re-minted, token useless
}

TEST_CASE("A room full of unverified filler still admits a real peer; a room of proven peers does not")
{
	rendezvous::ServerState state;
	constexpr std::string_view room = "WEDGE1";
	for (std::uint32_t host = 1; host <= 16; ++host)
	{
		Handshake(state, room, At(host, 1), 1000); // 16 unverified filler entries
	}

	const auto tokenReal = Handshake(state, room, At(99, 1), 1000); // displaces filler, not refused
	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, room, At(99, 1), tokenReal, "v1 10.0.0.1:1111", 1000, out);
	CHECK(out.empty()); // admitted and verified: alone, so nothing to forward

	constexpr std::string_view full = "VERIF1";
	for (std::uint32_t host = 1; host <= 16; ++host)
	{
		const auto token = Handshake(state, full, At(host + 200, 1), 1000);
		Publish(state, full, At(host + 200, 1), token, "v1 10.0.0.1:1111", 1000, out);
	}
	out.clear();
	rendezvous::ProcessDatagram(state, At(99, 1), FirstContact(full, "v1 6.6.6.6:66"), 1000, out);
	CHECK(out.empty()); // no slot to mint into: a full proven room answers nothing
}

TEST_CASE("A source that spends its outbound budget is throttled; other sources are not")
{
	rendezvous::ServerState state;
	constexpr std::string_view room = "FLOOD1";
	const auto tokenA = Handshake(state, room, At(1, 1), 1000);
	const auto tokenB = Handshake(state, room, At(2, 1), 1000);
	const std::string bigBlob = "v1 " + std::string(300, 'x');
	std::vector<rendezvous::OutboundDatagram> out;
	Publish(state, room, At(1, 1), tokenA, bigBlob, 1000, out);
	Publish(state, room, At(2, 1), tokenB, "v1 10.0.0.2:2222", 1000, out);
	out.clear();

	bool hitCap = false;
	for (int i = 0; i < 200 && !hitCap; ++i)
	{
		Publish(state, room, At(1, 1), tokenA, bigBlob, 1000, out);
		hitCap = out.empty();
		out.clear();
	}
	CHECK(hitCap); // one window, one source: the fan-out stops

	Publish(state, room, At(2, 1), tokenB, "v1 10.0.0.2:2223", 1000, out);
	CHECK(out.size() == 2); // an unrelated source is untouched by A's budget

	// The same cap bounds token handouts, which is all a spoofed-source flood can
	// ever buy: eight per window per claimed host, then silence.
	out.clear();
	const rendezvous::Address spoofer = At(3, 1);
	for (int i = 0; i < 10; ++i)
	{
		rendezvous::ProcessDatagram(state, spoofer, FirstContact("SPAM01", "v1 6.6.6.6:66"), 1000, out);
	}
	CHECK(out.size() == rendezvous::kMaxTokenRepliesPerSourcePerWindow);
}

TEST_CASE("Room codes normalize exactly as the client does")
{
	CHECK(rendezvous::NormalizeRoomCode("abcdef") == std::string("ABCDEF"));
	CHECK(rendezvous::NormalizeRoomCode("a b-c d ef") == std::string("ABCDEF"));
	CHECK(rendezvous::NormalizeRoomCode("o1i2l3") == std::string("011213")); // O->0, I/L->1
	CHECK(rendezvous::NormalizeRoomCode("ABCDE5") == std::string("ABCDE5"));
	CHECK_FALSE(rendezvous::NormalizeRoomCode("ABCDE").has_value());
	CHECK_FALSE(rendezvous::NormalizeRoomCode("ABCDEFG").has_value());
	CHECK_FALSE(rendezvous::NormalizeRoomCode("ABCDEU").has_value()); // U is outside the alphabet
	CHECK_FALSE(rendezvous::NormalizeRoomCode(std::string(65, 'a')).has_value());
}
