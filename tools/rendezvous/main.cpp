// aether-rendezvous: a small, long-running UDP server that pairs two NAT-traversal
// peers by a six-character room code and forwards the candidate blob each side
// publishes. It never sees game traffic - only these few-hundred-byte blobs - which
// is exactly what lets it stay this small and run unattended on a cheap VPS.
//
// Wire protocol, one datagram per line (verbatim, for the client side to match):
//   client -> server: "AECR1 <ROOMCODE> <blob>\n"
//   server -> client: "AECR1 <ROOMCODE> <blob>\n"  (the blob of a DIFFERENT peer in that room)
// <blob> is whatever aether::net::EncodeCandidates() produced; it contains spaces,
// so it is always the remainder of the line, never split on whitespace itself.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <enet/enet.h>

namespace
{
	constexpr std::string_view kMagic = "AECR1 ";
	constexpr std::uint16_t kDefaultPort = 24701;

	// A blob only ever holds up to kMaxCandidates (8) "dotted-quad:port" entries plus a
	// version tag (see Signaling.hpp), so a real message never comes close to this. A
	// datagram over the cap is dropped outright rather than parsed - an attacker gets
	// nothing for padding a packet.
	constexpr std::size_t kMaxDatagramBytes = 1200;

	// A room code is fixed at six characters after normalization; a raw token far
	// longer than that cannot possibly normalize to one, so it is rejected before the
	// per-character work below runs.
	constexpr std::size_t kMaxRawRoomTokenBytes = 64;

	constexpr std::size_t kRoomCodeLength = 6;
	constexpr std::string_view kRoomCodeAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; // Crockford base32, minus I/L/O/U

	// Bounds on live state so a burst of forged senders cannot grow memory without
	// limit: this server holds nothing but small in-memory maps, so its whole safety
	// story is capping and expiring them.
	constexpr std::size_t kMaxRooms = 4096;
	constexpr std::size_t kMaxPeersPerRoom = 16;
	constexpr std::uint32_t kIdleTimeoutMs = 120'000;
	constexpr std::uint32_t kSweepIntervalMs = 5'000;
	constexpr std::uint32_t kStatsIntervalMs = 30'000;
	constexpr std::uint32_t kWaitTimeoutMs = 1'000;

	std::atomic<bool> g_running{true};

	void HandleStopSignal(int /*signal*/)
	{
		// Signal-safe: only sets a flag the main loop polls after its next wake-up.
		g_running.store(false, std::memory_order_relaxed);
	}

	// Reimplements aether::net::NormalizeRoomCode's documented behavior standalone:
	// this tool deliberately links nothing from the engine (see CMakeLists.txt), so it
	// carries its own six lines rather than a dependency. Case-insensitive; maps the
	// confusable set (I/l -> 1, O -> 0) the alphabet excludes; strips spaces and
	// dashes; returns nullopt for anything that is not a room code once normalized.
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
		std::string_view blob;
	};

	// Refuses anything it does not fully understand rather than salvaging part of it,
	// matching the spirit of DecodeCandidates: a half-read line must never be forwarded
	// as if it were a real candidate blob.
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

		const std::string_view rest = line.substr(kMagic.size());
		const std::size_t spacePos = rest.find(' ');
		if (spacePos == std::string_view::npos)
		{
			return std::nullopt;
		}

		const std::string_view roomToken = rest.substr(0, spacePos);
		const std::string_view blob = rest.substr(spacePos + 1);
		if (roomToken.empty() || blob.empty())
		{
			return std::nullopt;
		}
		return ParsedMessage{roomToken, blob};
	}

	bool AddressesEqual(const ENetAddress& a, const ENetAddress& b)
	{
		return a.host == b.host && a.port == b.port;
	}

	struct Peer
	{
		ENetAddress address{};
		std::string blob;
		std::uint32_t lastSeenMs = 0;
	};

	struct Room
	{
		std::vector<Peer> peers;
	};

	using RoomMap = std::unordered_map<std::string, Room>;

	// Rooms live only as long as they hold at least one peer, so "room expires after
	// 120s idle" falls out of "every peer in it expired": there is no separate
	// room-level timer to keep in sync with the peer one.
	Room* FindOrCreateRoom(RoomMap& rooms, const std::string& code)
	{
		if (const auto it = rooms.find(code); it != rooms.end())
		{
			return &it->second;
		}
		if (rooms.size() >= kMaxRooms)
		{
			return nullptr;
		}
		return &rooms.emplace(code, Room{}).first->second;
	}

	Peer* FindOrAddPeer(Room& room, const ENetAddress& address, std::uint32_t now)
	{
		for (Peer& peer : room.peers)
		{
			if (AddressesEqual(peer.address, address))
			{
				return &peer;
			}
		}
		if (room.peers.size() >= kMaxPeersPerRoom)
		{
			return nullptr;
		}
		room.peers.push_back(Peer{address, {}, now});
		return &room.peers.back();
	}

	void SendLine(ENetSocket socket, const ENetAddress& to, std::string_view roomCode, std::string_view blob)
	{
		std::string line;
		line.reserve(kMagic.size() + roomCode.size() + 1 + blob.size() + 1);
		line.append(kMagic);
		line.append(roomCode);
		line.push_back(' ');
		line.append(blob);
		line.push_back('\n');

		ENetBuffer buffer{};
		buffer.data = line.data();
		buffer.dataLength = line.size();
		enet_socket_send(socket, &to, &buffer, 1);
	}

	// On a datagram: normalize the room code (drop if it fails), store the blob for
	// the sender, forward it to every OTHER peer in the room, and reply with those
	// peers' latest blobs so a late joiner learns what it missed in one round trip.
	void HandleDatagram(ENetSocket socket, RoomMap& rooms, const ENetAddress& from, std::string_view raw, std::uint32_t now)
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

		Room* room = FindOrCreateRoom(rooms, *roomCode);
		if (room == nullptr)
		{
			return; // At the room cap; a flood of distinct codes must not grow memory further.
		}
		Peer* sender = FindOrAddPeer(*room, from, now);
		if (sender == nullptr)
		{
			return; // Room already has as many peers as a punch session could ever use.
		}
		sender->blob = std::string(parsed->blob);
		sender->lastSeenMs = now;

		for (const Peer& peer : room->peers)
		{
			if (AddressesEqual(peer.address, from))
			{
				continue;
			}
			SendLine(socket, peer.address, *roomCode, sender->blob);
			SendLine(socket, from, *roomCode, peer.blob);
		}
	}

	void SweepExpired(RoomMap& rooms, std::uint32_t now)
	{
		for (auto it = rooms.begin(); it != rooms.end();)
		{
			std::erase_if(it->second.peers, [&](const Peer& peer) { return now - peer.lastSeenMs >= kIdleTimeoutMs; });
			if (it->second.peers.empty())
			{
				it = rooms.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void LogStats(const RoomMap& rooms)
	{
		std::size_t totalPeers = 0;
		for (const auto& [code, room] : rooms)
		{
			totalPeers += room.peers.size();
		}
		std::cout << "[rendezvous] rooms=" << rooms.size() << " peers=" << totalPeers << "\n";
	}

	void PrintUsage()
	{
		std::cout << "usage: aether-rendezvous [--port N]\n"
		             "  pairs NAT-traversal peers by room code and forwards their candidate blobs.\n"
		             "  carries no game traffic; open only this one UDP port to the internet.\n"
		             "  --port N   UDP port to listen on (default " << kDefaultPort << ")\n"
		             "  -h, --help show this message\n";
	}
} // namespace

int main(int argc, char** argv)
{
	std::uint16_t port = kDefaultPort;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg = argv[i];
		if (arg == "-h" || arg == "--help")
		{
			PrintUsage();
			return 0;
		}
		if (arg == "--port")
		{
			if (i + 1 >= argc)
			{
				std::cerr << "error: --port requires a value (try --help)\n";
				return 1;
			}
			const int parsedPort = std::atoi(argv[++i]);
			if (parsedPort <= 0 || parsedPort > 65535)
			{
				std::cerr << "error: --port must be between 1 and 65535\n";
				return 1;
			}
			port = static_cast<std::uint16_t>(parsedPort);
			continue;
		}
		std::cerr << "error: unrecognized argument '" << arg << "' (try --help)\n";
		return 1;
	}

	if (enet_initialize() != 0)
	{
		std::cerr << "error: enet_initialize failed\n";
		return 1;
	}

	const ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
	if (socket == ENET_SOCKET_NULL)
	{
		std::cerr << "error: enet_socket_create failed\n";
		enet_deinitialize();
		return 1;
	}
	// A restart must be able to rebind the same port immediately instead of waiting
	// out the OS's TIME_WAIT-style hold on the previous instance's socket.
	enet_socket_set_option(socket, ENET_SOCKOPT_REUSEADDR, 1);

	ENetAddress bindAddress{};
	bindAddress.host = ENET_HOST_ANY;
	bindAddress.port = port;
	if (enet_socket_bind(socket, &bindAddress) != 0)
	{
		std::cerr << "error: could not bind UDP port " << port << " (already in use?)\n";
		enet_socket_destroy(socket);
		enet_deinitialize();
		return 1;
	}

	std::signal(SIGINT, HandleStopSignal);
	std::signal(SIGTERM, HandleStopSignal);

	std::cout << "[rendezvous] listening on 0.0.0.0:" << port << " (UDP)\n";

	RoomMap rooms;
	std::vector<char> datagram(kMaxDatagramBytes);
	std::uint32_t lastSweepMs = enet_time_get();
	std::uint32_t lastStatsMs = lastSweepMs;

	while (g_running.load(std::memory_order_relaxed))
	{
		// Block with a timeout instead of polling: an idle rendezvous server - which is
		// most of the time, between punch attempts - must not spin a core for nothing.
		enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
		const int waitResult = enet_socket_wait(socket, &condition, kWaitTimeoutMs);
		if (waitResult == 0 && (condition & ENET_SOCKET_WAIT_RECEIVE) != 0)
		{
			ENetAddress from{};
			ENetBuffer buffer{};
			buffer.data = datagram.data();
			buffer.dataLength = datagram.size();
			const int received = enet_socket_receive(socket, &from, &buffer, 1);
			// A non-positive result covers "nothing to read" (a spurious wake), a
			// transient socket error, and an oversized datagram the OS refused to
			// deliver into our fixed buffer - all three are silently dropped rather
			// than logged, so a hostile sender flooding this port cannot fill the disk.
			if (received > 0)
			{
				HandleDatagram(socket, rooms, from, std::string_view(datagram.data(), static_cast<std::size_t>(received)), enet_time_get());
			}
		}

		const std::uint32_t now = enet_time_get();
		if (now - lastSweepMs >= kSweepIntervalMs)
		{
			SweepExpired(rooms, now);
			lastSweepMs = now;
		}
		if (now - lastStatsMs >= kStatsIntervalMs)
		{
			LogStats(rooms);
			lastStatsMs = now;
		}
	}

	std::cout << "[rendezvous] shutting down\n";
	enet_socket_destroy(socket);
	enet_deinitialize();
	return 0;
}
