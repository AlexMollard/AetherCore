#include "net/Signaling.hpp"

#include <charconv>
#include <sstream>
#include <utility>

namespace aether::net
{
	namespace
	{
		constexpr std::string_view kVersion = "v1";

		// A port has to survive the round trip exactly; from_chars is used rather than
		// stoi because it neither throws nor accepts the leading whitespace, plus signs
		// and trailing rubbish that would let "80abc" through as 80.
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
	} // namespace

	std::string EncodeCandidates(const CandidateSet& candidates)
	{
		std::ostringstream out;
		out << kVersion;
		std::size_t written = 0;
		for (const NatTraversal::Endpoint& endpoint: candidates.endpoints)
		{
			if (written >= kMaxCandidates)
			{
				break; // the cap is enforced on the way out too, not just on the way in
			}
			const std::string address = NatTraversal::FormatAddress(endpoint);
			if (address.empty())
			{
				continue;
			}
			out << ' ' << address << ':' << endpoint.port;
			++written;
		}
		return out.str();
	}

	std::optional<CandidateSet> DecodeCandidates(const std::string& text)
	{
		std::istringstream in(text);
		std::string token;
		if (!(in >> token) || token != kVersion)
		{
			// An unknown version is refused rather than guessed at. Whatever the field
			// means in that version, punching at an address read out of it under the
			// wrong rules is worse than not connecting.
			return std::nullopt;
		}

		CandidateSet out;
		while (in >> token)
		{
			if (out.endpoints.size() >= kMaxCandidates)
			{
				return std::nullopt; // a peer offering more than the cap is not one to trust
			}
			const std::size_t colon = token.rfind(':');
			if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size())
			{
				return std::nullopt;
			}
			const auto port = ParsePort(std::string_view{token}.substr(colon + 1));
			if (!port.has_value())
			{
				return std::nullopt;
			}
			const auto endpoint = NatTraversal::ParseEndpoint(token.substr(0, colon), *port);
			if (!endpoint.has_value())
			{
				return std::nullopt;
			}
			out.endpoints.push_back(*endpoint);
		}

		if (out.endpoints.empty())
		{
			return std::nullopt; // nothing to punch at is not a candidate set
		}
		return out;
	}

	void LocalSignalingChannel::Pair(LocalSignalingChannel& a, LocalSignalingChannel& b)
	{
		a.m_peer = &b;
		b.m_peer = &a;
	}

	void LocalSignalingChannel::Publish(const CandidateSet& candidates)
	{
		if (m_peer == nullptr)
		{
			return;
		}
		// Overwrites rather than queues: a later publish is a better answer than an
		// earlier one - it is the same peer with more candidates learned - and a queue
		// would hand the reader a stale set first.
		m_peer->m_inbox = EncodeCandidates(candidates);
	}

	std::optional<CandidateSet> LocalSignalingChannel::Poll()
	{
		if (!m_inbox.has_value())
		{
			return std::nullopt;
		}
		const std::string text = *std::exchange(m_inbox, std::nullopt);
		return DecodeCandidates(text);
	}
} // namespace aether::net
