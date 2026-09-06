#include "net/BroadcastSignaling.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>

#include <enet/enet.h>

#include "net/EnetInit.hpp"
#include "net/RoomCode.hpp"
#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		constexpr std::string_view kTag = "AECB1";
		constexpr std::size_t kNonceHexLength = 16;

		// How often an unanswered Publish is repeated. NatRendezvous - the only real
		// caller - fires Publish at most twice per session, once for its local
		// candidates at Begin() and once when the reflexive endpoint arrives (see
		// its Tick()); it never republishes on a timer of its own. So a peer that
		// starts listening after both of those has to hear something from here
		// instead, or a late joiner on the same LAN never learns anything at all.
		constexpr std::chrono::milliseconds kRepublishInterval{1000};

		// A single Poll() used to drain until the socket was empty, but "empty" is
		// remote-controlled: any device on the LAN can keep datagrams queued forever
		// (even garbage that fails ParseLine - the recv happens before the parse), and
		// Poll runs from the frame loop via NatRendezvous::Tick. Same bound as
		// RendezvousChannel's identical loop: read this many, leave the rest for the
		// next Poll().
		constexpr int kMaxDatagramsPerPoll = 64;

		std::uint64_t NowMs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		std::string RandomNonceHex()
		{
			std::random_device seedSource;
			std::mt19937_64 engine{(static_cast<std::uint64_t>(seedSource()) << 32) | seedSource()};
			const std::uint64_t value = engine();
			std::array<char, kNonceHexLength + 1> text{};
			std::snprintf(text.data(), text.size(), "%016llx", static_cast<unsigned long long>(value));
			return std::string(text.data(), kNonceHexLength);
		}

		// Consumes one whitespace-delimited token from `line`, advancing `pos` past
		// it and the whitespace that follows. False means the line ran out before a
		// token was found - never true with an empty token, so a caller never has to
		// separately check for that.
		bool NextToken(std::string_view line, std::size_t& pos, std::string_view& out)
		{
			while (pos < line.size() && line[pos] == ' ')
			{
				++pos;
			}
			const std::size_t start = pos;
			while (pos < line.size() && line[pos] != ' ')
			{
				++pos;
			}
			if (pos == start)
			{
				return false;
			}
			out = line.substr(start, pos - start);
			return true;
		}
	} // namespace

	BroadcastSignalingChannel::BroadcastSignalingChannel(std::string roomCode, std::uint16_t port)
	      : m_port(port)
	{
		const auto normalized = NormalizeRoomCode(roomCode);
		if (!normalized.has_value())
		{
			Fail("not a room code: '" + roomCode + "'");
			return;
		}
		m_roomCode = *normalized;
		m_nonceHex = RandomNonceHex();

		if (!AcquireEnet())
		{
			Fail("enet_initialize failed");
			return;
		}

		const ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
		if (socket == ENET_SOCKET_NULL)
		{
			ReleaseEnet();
			Fail("could not create a UDP socket");
			return;
		}

		// REUSEADDR must be set on THIS socket BEFORE this socket's own bind() call -
		// that is the only point at which the OS decides whether this bind may share
		// a port another socket already holds, so setting it any later is a no-op for
		// the very case it exists for. (This used to be set after bind, inside one
		// `||` chain with bind and the other options; the second instance on a
		// machine always failed to bind because ITS OWN REUSEADDR was not yet set
		// when ITS OWN bind() ran, and a chain whose correctness depends on
		// evaluation order is exactly how that went unnoticed - split out on
		// purpose, so nobody can restore the bug by reordering operands.) Not a
		// hijack vector worth closing: a process that can bind inside this machine
		// can do far worse to it than eat a broadcast, and dropping reuse would
		// break the two-instances-on-one-machine case outright.
		if (enet_socket_set_option(socket, ENET_SOCKOPT_REUSEADDR, 1) != 0)
		{
			enet_socket_destroy(socket);
			ReleaseEnet();
			Fail("could not set SO_REUSEADDR on the broadcast socket");
			return;
		}

		// Bound to ANY:port so this instance RECEIVES the room's broadcasts too, not
		// only sends them. REUSEADDR is already set above, before this call, which is
		// what lets a second instance on this same machine (two local test peers, or
		// a crashed session's process whose socket the OS has not yet reaped) bind
		// here too instead of failing for no reason a player could act on.
		ENetAddress bindAddress{};
		bindAddress.host = ENET_HOST_ANY;
		bindAddress.port = m_port;
		if (enet_socket_bind(socket, &bindAddress) != 0)
		{
			enet_socket_destroy(socket);
			ReleaseEnet();
			// A firewall blocking the bind and a port already claimed by something
			// else (with no REUSEADDR set on THAT socket, or from a process outside
			// this engine entirely) both land here, and either is something a
			// player can be told - unlike a crash, which is not.
			Fail("could not bind a broadcast socket on port " + std::to_string(m_port));
			return;
		}

		// Non-blocking so Poll() can drain without ever stalling the frame loop.
		// Order relative to bind does not matter for this one - unlike REUSEADDR,
		// it is not a port-sharing permission the bind syscall itself consults.
		if (enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1) != 0)
		{
			enet_socket_destroy(socket);
			ReleaseEnet();
			Fail("could not set the broadcast socket non-blocking");
			return;
		}

		// Broadcast-enabled so Publish's send is even allowed to leave the
		// interface. Same as NONBLOCK above: no bind-ordering dependency.
		if (enet_socket_set_option(socket, ENET_SOCKOPT_BROADCAST, 1) != 0)
		{
			enet_socket_destroy(socket);
			ReleaseEnet();
			Fail("could not enable broadcast on the socket for port " + std::to_string(m_port));
			return;
		}

		m_socket = static_cast<std::intptr_t>(socket);
		m_usable = true;
	}

	BroadcastSignalingChannel::~BroadcastSignalingChannel()
	{
		if (m_socket != -1)
		{
			enet_socket_destroy(static_cast<ENetSocket>(m_socket));
			ReleaseEnet();
		}
	}

	void BroadcastSignalingChannel::Fail(std::string reason)
	{
		m_usable = false;
		m_failure = std::move(reason);
		AE_WARN(LogCategory::App, "LAN broadcast signalling: {}", m_failure);
	}

	void BroadcastSignalingChannel::SendNow(const CandidateSet& candidates)
	{
		std::string line;
		line.reserve(kTag.size() + 1 + m_roomCode.size() + 1 + m_nonceHex.size() + 1 + 128);
		line += kTag;
		line += ' ';
		line += m_roomCode;
		line += ' ';
		line += m_nonceHex;
		line += ' ';
		line += EncodeCandidates(candidates);
		line += '\n';

		ENetAddress to{};
		to.host = ENET_HOST_BROADCAST;
		to.port = m_port;

		ENetBuffer buffer{};
		buffer.data = line.data();
		buffer.dataLength = line.size();
		// The result is not checked: a dropped broadcast is exactly what the
		// republish timer in Poll() exists to paper over, and every candidate this
		// channel ever offers already gets sent again within a second.
		enet_socket_send(static_cast<ENetSocket>(m_socket), &to, &buffer, 1);
		m_lastSendMs = NowMs();
	}

	void BroadcastSignalingChannel::Publish(const CandidateSet& candidates)
	{
		if (!m_usable)
		{
			return;
		}
		m_lastPublished = candidates;
		SendNow(candidates);
	}

	std::optional<CandidateSet> BroadcastSignalingChannel::Poll()
	{
		if (!m_usable)
		{
			return std::nullopt;
		}

		if (m_lastPublished.has_value() && NowMs() - m_lastSendMs >= static_cast<std::uint64_t>(kRepublishInterval.count()))
		{
			SendNow(*m_lastPublished);
		}

		CandidateSet merged;
		bool any = false;
		std::array<char, kMaxLineLength> buffer{};
		for (int i = 0; i < kMaxDatagramsPerPoll; ++i)
		{
			ENetAddress from{};
			ENetBuffer recvBuffer{};
			recvBuffer.data = buffer.data();
			recvBuffer.dataLength = buffer.size();
			const int received = enet_socket_receive(static_cast<ENetSocket>(m_socket), &from, &recvBuffer, 1);
			if (received == 0)
			{
				break; // nothing waiting - never block the frame loop on this
			}
			if (received < 0)
			{
				if (received == -2)
				{
					continue; // one datagram too big for the buffer; more may follow
				}
				break; // a real socket error; leave it for next frame
			}

			const std::string_view line(buffer.data(), static_cast<std::size_t>(received));
			const auto candidates = ParseLine(line, m_roomCode, m_nonceHex);
			if (!candidates.has_value())
			{
				continue;
			}
			for (const NatTraversal::Endpoint& endpoint: candidates->endpoints)
			{
				if (merged.endpoints.size() >= kMaxCandidates)
				{
					break;
				}
				if (std::ranges::find(merged.endpoints, endpoint) == merged.endpoints.end())
				{
					merged.endpoints.push_back(endpoint);
					any = true;
				}
			}
		}

		if (!any)
		{
			return std::nullopt;
		}
		return merged;
	}

	std::optional<CandidateSet> BroadcastSignalingChannel::ParseLine(std::string_view line, std::string_view roomCode, std::string_view ownNonceHex)
	{
		// Length is checked before anything else touches the line: a peer that can
		// flood letters onto the wire must not make this machine do proportionally
		// more work parsing them than any other datagram would cost.
		if (line.empty() || line.size() > kMaxLineLength)
		{
			return std::nullopt;
		}
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
		{
			line.remove_suffix(1);
		}

		std::size_t pos = 0;
		std::string_view tag;
		std::string_view codeToken;
		std::string_view nonceToken;
		if (!NextToken(line, pos, tag) || tag != kTag)
		{
			return std::nullopt;
		}
		if (!NextToken(line, pos, codeToken))
		{
			return std::nullopt;
		}
		if (!NextToken(line, pos, nonceToken))
		{
			return std::nullopt;
		}

		const auto normalizedCode = NormalizeRoomCode(codeToken);
		if (!normalizedCode.has_value() || *normalizedCode != roomCode)
		{
			return std::nullopt; // not a room code, or a different room entirely
		}

		const bool nonceLooksRight = nonceToken.size() == kNonceHexLength && std::ranges::all_of(nonceToken, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
		if (!nonceLooksRight)
		{
			return std::nullopt;
		}
		if (nonceToken == ownNonceHex)
		{
			return std::nullopt; // this channel's own broadcast, heard on its own socket
		}

		// Whatever remains after exactly one more space is the blob, taken whole:
		// EncodeCandidates' own output contains spaces, so splitting further here
		// would cut a well-formed candidate list in half.
		if (pos >= line.size() || line[pos] != ' ')
		{
			return std::nullopt; // no blob at all
		}
		++pos;
		const std::string_view blob = line.substr(pos);
		if (blob.empty())
		{
			return std::nullopt;
		}
		return DecodeCandidates(std::string(blob));
	}
} // namespace aether::net
