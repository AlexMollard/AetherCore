// aether-rendezvous: a small, long-running UDP server that pairs two NAT-traversal
// peers by a six-character room code and forwards the candidate blob each side
// publishes. It never sees game traffic - only these few-hundred-byte blobs - which
// is exactly what lets it stay this small and run unattended on a cheap VPS.
//
// The whole policy - grammar, room/peer tables, capability tokens, abuse bounds -
// lives in RendezvousServerLogic.hpp so it is unit-testable without a socket; this
// file is only the socket loop that pumps datagrams through it.
//
// Wire protocol (AECR2; one datagram per line, verbatim for the client side):
//   client -> server: "AECR2 <ROOM> <TOKEN> <blob>\n"   (TOKEN "-" until issued)
//   server -> client: "AECR2 <ROOM> <TOKEN> <body>\n"   (TOKEN = recipient's own;
//                                                         body "-" or a peer's blob)
// <blob> is whatever aether::net::EncodeCandidates() produced; it contains spaces,
// so it is always the remainder of the line, never split on whitespace itself.
// A peer must complete one token round trip before the server will store or
// forward its blob, which is what keeps this open port from being usable as a
// reflection amplifier or for poisoning another peer's candidates - see the
// comments in the header for the exact rules.

#include "RendezvousServerLogic.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <enet/enet.h>

namespace
{
	std::atomic<bool> g_running{true};

	void HandleStopSignal(int /*signal*/)
	{
		// Signal-safe: only sets a flag the main loop polls after its next wake-up.
		g_running.store(false, std::memory_order_relaxed);
	}

	void DispatchOutbound(ENetSocket socket, const std::vector<rendezvous::OutboundDatagram>& outbound)
	{
		for (const rendezvous::OutboundDatagram& datagram : outbound)
		{
			ENetAddress to{};
			to.host = datagram.to.host;
			to.port = datagram.to.port;
			ENetBuffer buffer{};
			buffer.data = const_cast<char*>(datagram.line.data());
			buffer.dataLength = datagram.line.size();
			enet_socket_send(socket, &to, &buffer, 1);
		}
	}

	void LogStats(const rendezvous::ServerState& state)
	{
		std::size_t totalPeers = 0;
		for (const auto& [code, room] : state.rooms)
		{
			totalPeers += room.peers.size();
		}
		std::cout << "[rendezvous] rooms=" << state.rooms.size() << " peers=" << totalPeers
		          << " sources=" << state.sources.size() << "\n";
	}

	void PrintUsage()
	{
		std::cout << "usage: aether-rendezvous [--port N]\n"
		             "  pairs NAT-traversal peers by room code and forwards their candidate blobs.\n"
		             "  carries no game traffic; open only this one UDP port to the internet.\n"
		             "  --port N   UDP port to listen on (default " << rendezvous::kDefaultPort << ")\n"
		             "  -h, --help show this message\n";
	}
} // namespace

int main(int argc, char** argv)
{
	std::uint16_t port = rendezvous::kDefaultPort;
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

	rendezvous::ServerState state;
	std::vector<char> datagram(rendezvous::kMaxDatagramBytes);
	std::vector<rendezvous::OutboundDatagram> outbound;
	std::uint32_t lastSweepMs = enet_time_get();
	std::uint32_t lastStatsMs = lastSweepMs;

	while (g_running.load(std::memory_order_relaxed))
	{
		// Block with a timeout instead of polling: an idle rendezvous server - which is
		// most of the time, between punch attempts - must not spin a core for nothing.
		enet_uint32 condition = ENET_SOCKET_WAIT_RECEIVE;
		const int waitResult = enet_socket_wait(socket, &condition, rendezvous::kWaitTimeoutMs);
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
				outbound.clear();
				rendezvous::ProcessDatagram(state,
				                           rendezvous::Address{from.host, from.port},
				                           std::string_view(datagram.data(), static_cast<std::size_t>(received)),
				                           enet_time_get(),
				                           outbound);
				DispatchOutbound(socket, outbound);
			}
		}

		const std::uint32_t now = enet_time_get();
		if (now - lastSweepMs >= rendezvous::kSweepIntervalMs)
		{
			rendezvous::SweepExpired(state, now);
			lastSweepMs = now;
		}
		if (now - lastStatsMs >= rendezvous::kStatsIntervalMs)
		{
			LogStats(state);
			lastStatsMs = now;
		}
	}

	std::cout << "[rendezvous] shutting down\n";
	enet_socket_destroy(socket);
	enet_deinitialize();
	return 0;
}
