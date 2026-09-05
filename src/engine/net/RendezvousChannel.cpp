#include "net/RendezvousChannel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <utility>

#include <enet/enet.h>

#include "net/EnetInit.hpp"
#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// The 5-byte version token. Followed by exactly one space before the room
		// code, so together with that separator this is the same 6-byte magic the
		// server checks for - just implemented as split-then-compare rather than a
		// fixed-prefix match, which reads the same either way. AECR1 is this
		// protocol's dead ancestor: that one had no token round trip, which made
		// the server a reflection amplifier, so a server that still speaks it (or a
		// client that still sends it) is refused unread rather than half-worked
		// with.
		constexpr std::string_view kVersion = "AECR2";

		// "No token yet" - what a first contact sends, and the only other value the
		// token field may hold besides sixteen hex characters.
		constexpr std::string_view kNoToken = "-";
		constexpr std::size_t kTokenHexLength = 16;

		// How often an unacknowledged publish goes out again, and how many times
		// before giving up. UDP drops datagrams and the server may not have this
		// room's other peer registered on the first try, so one send proves nothing;
		// equally, a room nobody else ever joins must not be re-sent into forever.
		constexpr std::chrono::seconds kRetryInterval{2};
		constexpr int kMaxRetryAttempts = 30;

		// A single Poll() call drains the socket until it is empty, which is the
		// point - but "empty" is remote-controlled: nothing stops a hostile relay
		// from keeping datagrams queued forever. This bounds how many one call reads
		// before yielding back to the frame loop; the rest is still there next Poll().
		constexpr int kMaxDatagramsPerPoll = 64;

		// A port has to survive the round trip exactly; from_chars is used rather
		// than stoi because it neither throws nor accepts leading whitespace, a sign,
		// or trailing rubbish that would let "24701abc" through as 24701.
		std::optional<std::uint16_t> ParsePort(std::string_view text)
		{
			unsigned int value = 0;
			const char* begin = text.data();
			const char* end = begin + text.size();
			const auto result = std::from_chars(begin, end, value);
			if (result.ec != std::errc{} || result.ptr != end)
			{
				return std::nullopt;
			}
			if (value == 0 || value > 65535)
			{
				return std::nullopt;
			}
			return static_cast<std::uint16_t>(value);
		}

		// A token has to be exactly `-` or sixteen hex characters; the server emits
		// uppercase and accepts either case, and so does this parse.
		[[nodiscard]] bool IsValidToken(std::string_view token)
		{
			if (token == kNoToken)
			{
				return true;
			}
			return token.size() == kTokenHexLength && std::ranges::all_of(token, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
		}

		struct HostPort
		{
			std::string host;
			std::uint16_t port = 0;
		};

		// "host:port", or a bare host defaulting to kDefaultRendezvousPort. IPv4
		// dotted-quads and DNS hostnames never contain a colon, so the last one -
		// there is at most one - unambiguously separates the two fields.
		std::optional<HostPort> SplitHostPort(std::string_view address)
		{
			const std::size_t colon = address.rfind(':');
			if (colon == std::string_view::npos)
			{
				if (address.empty())
				{
					return std::nullopt;
				}
				return HostPort{std::string(address), kDefaultRendezvousPort};
			}

			const std::string_view hostPart = address.substr(0, colon);
			if (hostPart.empty())
			{
				return std::nullopt;
			}
			const auto port = ParsePort(address.substr(colon + 1));
			if (!port)
			{
				return std::nullopt;
			}
			return HostPort{std::string(hostPart), *port};
		}
	} // namespace

	std::string EncodeRendezvousDatagram(std::string_view roomCode, std::string_view token, std::string_view blob)
	{
		std::string out;
		out.reserve(kVersion.size() + 1 + roomCode.size() + 1 + token.size() + 1 + blob.size() + 1);
		out += kVersion;
		out += ' ';
		out += roomCode;
		out += ' ';
		out += token;
		out += ' ';
		out += blob;
		out += '\n';
		return out;
	}

	std::optional<RendezvousDatagram> ParseRendezvousDatagram(std::string_view datagram)
	{
		if (datagram.empty() || datagram.size() > kMaxRendezvousDatagramLength)
		{
			return std::nullopt;
		}

		// The server tolerates a bare LF and a full CRLF alike; strip whichever
		// terminates the line before anything else is inspected.
		std::string_view line = datagram;
		if (line.back() == '\n')
		{
			line.remove_suffix(1);
			if (!line.empty() && line.back() == '\r')
			{
				line.remove_suffix(1);
			}
		}

		// Anything left containing a line terminator is more than one line in one
		// datagram, which this protocol never sends - refused rather than guessed at.
		if (line.find('\n') != std::string_view::npos || line.find('\r') != std::string_view::npos)
		{
			return std::nullopt;
		}

		// Four space-separated fields, the last of which may itself contain spaces -
		// so the leading three are split off and the rest is taken whole, exactly like
		// the server does it.
		std::array<std::string_view, 3> fields{};
		std::size_t pos = 0;
		for (std::size_t i = 0; i < fields.size(); ++i)
		{
			const std::size_t space = line.find(' ', pos);
			if (space == std::string_view::npos)
			{
				return std::nullopt; // fewer than four fields - a truncated line
			}
			fields[i] = line.substr(pos, space - pos);
			pos = space + 1;
		}
		if (pos >= line.size())
		{
			return std::nullopt; // the fourth field exists as a separator but is empty
		}

		if (fields[0] != kVersion)
		{
			return std::nullopt; // wrong or absent version, AECR1 included
		}
		if (!IsValidToken(fields[2]))
		{
			return std::nullopt; // neither "-" nor sixteen hex characters
		}

		RendezvousDatagram out;
		out.roomCode = std::string(fields[1]);
		out.token = std::string(fields[2]);
		out.body = std::string(line.substr(pos));
		return out;
	}

	std::optional<CandidateSet> InterpretRendezvousDatagram(std::string_view datagram, std::string_view expectedRoomCode)
	{
		const auto parsed = ParseRendezvousDatagram(datagram);
		if (!parsed || parsed->roomCode != expectedRoomCode)
		{
			return std::nullopt;
		}
		// The handshake-only body is the server saying "here is your token, nothing
		// else" - that is not candidates, and reading it as a blob would only produce
		// a decode failure that means nothing.
		if (parsed->body == kNoToken)
		{
			return std::nullopt;
		}
		return DecodeCandidates(parsed->body);
	}

	bool AccumulateCandidates(std::vector<NatTraversal::Endpoint>& accumulator, const CandidateSet& incoming)
	{
		bool grew = false;
		for (const NatTraversal::Endpoint& candidate: incoming.endpoints)
		{
			if (accumulator.size() >= kMaxCandidates)
			{
				break;
			}
			if (std::ranges::find(accumulator, candidate) == accumulator.end())
			{
				accumulator.push_back(candidate);
				grew = true;
			}
		}
		return grew;
	}

	RendezvousChannel::RendezvousChannel(std::string serverAddress, std::string roomCode)
	      : m_roomCode(std::move(roomCode))
	{
		const auto split = SplitHostPort(serverAddress);
		if (!split)
		{
			Fail("rendezvous server address '" + serverAddress + "' is not host[:port]");
			return;
		}

		if (!AcquireEnet())
		{
			Fail("enet_initialize failed");
			return;
		}
		m_enetAcquired = true;

		ENetAddress resolved{};
		if (enet_address_set_host(&resolved, split->host.c_str()) != 0)
		{
			Fail("could not resolve rendezvous server '" + split->host + "'");
			return;
		}
		m_serverHost = resolved.host;
		m_serverPort = split->port;

		const ENetSocket socket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
		if (socket == ENET_SOCKET_NULL)
		{
			Fail("could not create a socket for rendezvous signalling");
			return;
		}

		// A socket of our own, never the transport's - see the header on
		// NatTraversal for why the punch itself must never bind a second one. Bound
		// to an ephemeral port so it can receive before the first send has to happen;
		// nobody needs to know what port it landed on.
		ENetAddress any{};
		any.host = ENET_HOST_ANY;
		any.port = ENET_PORT_ANY;
		if (enet_socket_bind(socket, &any) != 0 || enet_socket_set_option(socket, ENET_SOCKOPT_NONBLOCK, 1) != 0)
		{
			enet_socket_destroy(socket);
			Fail("could not prepare the rendezvous signalling socket");
			return;
		}

		static_assert(sizeof(ENetSocket) <= sizeof(SocketHandle), "ENetSocket must fit in the pointer-width handle stored in the header");
		m_socket = static_cast<SocketHandle>(socket);
		m_usable = true;
	}

	RendezvousChannel::~RendezvousChannel()
	{
		if (m_socket != kInvalidSocket)
		{
			enet_socket_destroy(static_cast<ENetSocket>(m_socket));
		}
		if (m_enetAcquired)
		{
			ReleaseEnet();
		}
	}

	void RendezvousChannel::Fail(std::string reason)
	{
		m_usable = false;
		m_failure = std::move(reason);
		AE_WARN(LogCategory::App, "Rendezvous channel failed: {}", m_failure);
	}

	void RendezvousChannel::SendDatagram(std::string_view body)
	{
		ENetAddress to{};
		to.host = m_serverHost;
		to.port = m_serverPort;

		// The token goes on every datagram, `-` until the server has issued one:
		// without it the server ignores the body outright, so first contact is
		// always a token round trip (see the grammar comment in the header).
		const std::string_view token = m_token.empty() ? std::string_view{kNoToken} : std::string_view{m_token};
		const std::string datagram = EncodeRendezvousDatagram(m_roomCode, token, body);

		ENetBuffer wire{};
		wire.data = const_cast<char*>(datagram.data());
		wire.dataLength = datagram.size();
		enet_socket_send(static_cast<ENetSocket>(m_socket), &to, &wire, 1);
	}

	void RendezvousChannel::MaybeRetransmit()
	{
		if (!m_outboundBlob.has_value() || !m_awaitingFirstReply || m_retriesLeft <= 0)
		{
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - m_lastSend < kRetryInterval)
		{
			return;
		}
		SendDatagram(*m_outboundBlob);
		m_lastSend = now;
		--m_retriesLeft;
	}

	void RendezvousChannel::Publish(const CandidateSet& candidates)
	{
		if (!m_usable)
		{
			return;
		}

		std::string blob = EncodeCandidates(candidates);
		m_lastSend = std::chrono::steady_clock::now();
		SendDatagram(blob);

		// A later Publish() carries genuinely new information - the local candidates
		// were known immediately, the public one only once STUN answers - so it
		// starts its own fresh attempt budget rather than inheriting what was left.
		m_outboundBlob = std::move(blob);
		m_awaitingFirstReply = true;
		m_retriesLeft = kMaxRetryAttempts;
	}

	std::optional<CandidateSet> RendezvousChannel::Poll()
	{
		if (!m_usable)
		{
			return std::nullopt;
		}

		MaybeRetransmit();

		bool grew = false;
		bool sawReply = false;
		std::array<char, kMaxRendezvousDatagramLength> buffer{};
		for (int i = 0; i < kMaxDatagramsPerPoll; ++i)
		{
			ENetAddress from{};
			ENetBuffer wire{};
			wire.data = buffer.data();
			wire.dataLength = buffer.size();
			const int received = enet_socket_receive(static_cast<ENetSocket>(m_socket), &from, &wire, 1);
			if (received == 0)
			{
				break; // nothing left waiting
			}
			if (received < 0)
			{
				if (received == -2)
				{
					continue; // this one datagram was oversized and already discarded by the OS; more may follow
				}
				break; // a real socket error - try again on the next Poll()
			}

			const std::string_view datagram(buffer.data(), static_cast<std::size_t>(received));
			// Only the configured server may speak for this room. The socket is bound
			// to ANY:ephemeral and the room code is public (the host itself broadcasts
			// it), so grammar and code alone prove nothing about who sent a datagram -
			// accepting one from any other source would let a single spoofed line both
			// freeze the local publish (sawReply below) and aim punches, and later
			// relays, at addresses nobody offered. Checked before anything is parsed.
			if (from.host != m_serverHost || from.port != m_serverPort)
			{
				continue;
			}

			const auto parsed = ParseRendezvousDatagram(datagram);
			if (!parsed || parsed->roomCode != m_roomCode)
			{
				continue;
			}
			// The server puts this peer's own token in every line it sends. Storing
			// it on receipt is what makes the NEXT publish land: a datagram sent
			// without it is ignored outright, blob and all.
			if (parsed->token != kNoToken && parsed->token != m_token)
			{
				m_token = parsed->token;
				// A token-only answer means the server heard us but could not accept
				// what we sent - it arrived without the token it is answering with.
				// The outstanding blob (if any) goes back out right away, now with
				// it, rather than waiting out the retransmit timer: the server is
				// demonstrably listening, and a two-second pause here is a two-second
				// delay on every first publish of a session.
				if (m_outboundBlob.has_value() && m_awaitingFirstReply && m_retriesLeft > 0)
				{
					SendDatagram(*m_outboundBlob);
					m_lastSend = std::chrono::steady_clock::now();
					--m_retriesLeft;
				}
			}
			if (parsed->body == kNoToken)
			{
				continue; // handshake-only: a token, deliberately nothing else
			}
			if (const auto offered = DecodeCandidates(parsed->body))
			{
				sawReply = true;
				grew = AccumulateCandidates(m_accumulated, *offered) || grew;
			}
		}

		if (sawReply)
		{
			// The server has this room and this peer registered - proven by the fact
			// that it sent something back - so hammering it with retries no longer
			// buys anything until the next genuinely new Publish().
			m_awaitingFirstReply = false;
		}

		if (!grew)
		{
			return std::nullopt;
		}
		CandidateSet result;
		result.endpoints = m_accumulated;
		return result;
	}
} // namespace aether::net
