#include "net/NatTraversal.hpp"

#include <algorithm>
#include <map>
#include <mutex>

#include <enet/enet.h>

#include "utils/Logger.hpp"

namespace aether::net
{
	namespace
	{
		// ENet gives the intercept callback nothing but the host, and ENetHost has no
		// user-data field to hang an owner off. So the owner is looked up here.
		std::mutex g_registryMutex;
		std::map<_ENetHost*, NatTraversal*>& Registry()
		{
			static std::map<_ENetHost*, NatTraversal*> registry;
			return registry;
		}

		// How often an unanswered request goes out again, and how long before giving up.
		// UDP drops datagrams and a punch is a race between two NATs, so one send proves
		// nothing; equally, retrying forever leaves a player staring at a spinner when
		// the honest answer is that their NAT cannot do this.
		constexpr float kRetryInterval = 0.25f;
		constexpr float kDiscoveryTimeout = 5.0f;
		constexpr float kPunchTimeout = 10.0f;

		stun::Endpoint ToStun(const NatTraversal::Endpoint& e)
		{
			// ENet keeps `host` in network order and `port` in host order; STUN wants
			// both in host order.
			return stun::Endpoint{ENET_NET_TO_HOST_32(e.host), e.port};
		}

		NatTraversal::Endpoint FromStun(const stun::Endpoint& e)
		{
			return NatTraversal::Endpoint{ENET_HOST_TO_NET_32(e.address), e.port};
		}
	} // namespace

	NatTraversal::NatTraversal(_ENetHost* host)
	      : m_host(host)
	{
		if (m_host == nullptr)
		{
			Fail("no ENet host");
			return;
		}
		{
			const std::scoped_lock lock(g_registryMutex);
			Registry()[m_host] = this;
		}
		// Exact signature, no cast: calling through a function pointer cast to a
		// different parameter type is undefined, and ENET_CALLBACK carries a calling
		// convention on some targets that a cast would quietly discard.
		m_host->intercept = &NatTraversal::InterceptThunk;
	}

	NatTraversal::~NatTraversal()
	{
		if (m_host == nullptr)
		{
			return;
		}
		// Cleared before deregistering: leaving the callback installed against a freed
		// owner is a use-after-free on the next datagram, which would arrive during an
		// ordinary poll long after anyone was thinking about traversal.
		m_host->intercept = nullptr;
		const std::scoped_lock lock(g_registryMutex);
		Registry().erase(m_host);
	}

	int NatTraversal::InterceptThunk(_ENetHost* host, _ENetEvent* /*event*/)
	{
		NatTraversal* self = nullptr;
		{
			const std::scoped_lock lock(g_registryMutex);
			const auto it = Registry().find(host);
			if (it == Registry().end())
			{
				return 0;
			}
			self = it->second;
		}

		const Endpoint from{host->receivedAddress.host, host->receivedAddress.port};
		const std::span<const std::byte> data{reinterpret_cast<const std::byte*>(host->receivedData), host->receivedDataLength};
		return self->OnDatagram(from, data) ? 1 : 0;
	}

	void NatTraversal::Send(const Endpoint& to, std::span<const std::uint8_t> bytes)
	{
		ENetAddress address{};
		address.host = to.host;
		address.port = to.port;

		ENetBuffer buffer{};
		buffer.data = const_cast<std::uint8_t*>(bytes.data());
		buffer.dataLength = bytes.size();
		enet_socket_send(m_host->socket, &address, &buffer, 1);
	}

	void NatTraversal::Fail(std::string reason)
	{
		m_state = State::Failed;
		m_failure = std::move(reason);
		AE_WARN(LogCategory::App, "NAT traversal failed: {}", m_failure);
	}

	bool NatTraversal::BeginDiscovery(const std::string& stunHost, std::uint16_t stunPort)
	{
		if (m_host == nullptr)
		{
			return false;
		}

		ENetAddress resolved{};
		if (enet_address_set_host(&resolved, stunHost.c_str()) != 0)
		{
			Fail("could not resolve STUN server '" + stunHost + "'");
			return false;
		}
		m_stunServer = Endpoint{resolved.host, stunPort};
		m_discoveryId = stun::MakeTransactionId();
		m_public.reset();
		m_state = State::Discovering;
		m_elapsed = 0.0f;
		m_sinceRetry = kRetryInterval; // send immediately on the next Tick
		m_attempts = 0;
		return true;
	}

	void NatTraversal::BeginPunch(std::span<const Endpoint> peerCandidates)
	{
		if (m_host == nullptr || peerCandidates.empty())
		{
			Fail("no peer candidates to punch toward");
			return;
		}

		// Every candidate gets its own transaction id, so the answer identifies which
		// path opened rather than only that one of them did.
		m_checks.clear();
		m_checks.reserve(peerCandidates.size());
		for (const Endpoint& candidate: peerCandidates)
		{
			m_checks.push_back(Check{candidate, stun::MakeTransactionId()});
		}
		m_open.reset();
		m_state = State::Punching;
		m_elapsed = 0.0f;
		m_sinceRetry = kRetryInterval;
		m_attempts = 0;
	}

	bool NatTraversal::OnDatagram(const Endpoint& from, std::span<const std::byte> data)
	{
		switch (stun::Classify(data))
		{
			case stun::MessageKind::BindingRequest:
			{
				// A peer checking whether it can reach us. Answering is half of the
				// punch: our reply is what tells it the path works, and the request
				// itself has already opened our side of the hole toward it.
				const auto id = stun::ReadTransactionId(data);
				if (!id.has_value())
				{
					return false;
				}
				const auto response = stun::BuildBindingResponse(*id, ToStun(from));
				Send(from, response);
				return true;
			}
			case stun::MessageKind::BindingSuccess:
			{
				if (m_state == State::Discovering)
				{
					if (const auto reflexive = stun::ParseBindingResponse(data, m_discoveryId))
					{
						m_public = FromStun(*reflexive);
						m_state = State::Discovered;
						AE_INFO(LogCategory::App, "NAT traversal: public endpoint discovered on this socket.");
						return true;
					}
					return false;
				}
				if (m_state == State::Punching)
				{
					// Matched by transaction id, not by source address: a NAT can
					// answer from a different port than the one written to, and
					// trusting the source would drop the very reply being waited for.
					const auto match = std::ranges::find_if(m_checks, [&](const Check& c) { return stun::ParseBindingResponse(data, c.id).has_value(); });
					if (match == m_checks.end())
					{
						return false;
					}
					m_open = from;
					m_state = State::Open;
					AE_INFO(LogCategory::App, "NAT traversal: a path to the peer is open.");
					return true;
				}
				return false;
			}
			case stun::MessageKind::Other:
				break;
		}

		// Anything else is ENet's, or nobody's. Returning false leaves it to ENet, which
		// is the only safe default: swallowing a datagram that was not ours would look
		// exactly like packet loss to the layer that was waiting for it.
		return false;
	}

	void NatTraversal::Tick(float deltaSeconds)
	{
		if (m_state != State::Discovering && m_state != State::Punching)
		{
			return;
		}

		m_elapsed += deltaSeconds;
		m_sinceRetry += deltaSeconds;

		const float timeout = m_state == State::Discovering ? kDiscoveryTimeout : kPunchTimeout;
		if (m_elapsed >= timeout)
		{
			if (m_state == State::Discovering)
			{
				Fail("the STUN server did not answer; this socket's public address is unknown");
			}
			else
			{
				// Named specifically, because "connection failed" sends a player
				// hunting through their router for a setting that will not help.
				Fail("no path to the peer opened - a NAT on one side is likely symmetric, which needs a relay rather than port forwarding");
			}
			return;
		}

		if (m_sinceRetry < kRetryInterval)
		{
			return;
		}
		m_sinceRetry = 0.0f;
		++m_attempts;

		if (m_state == State::Discovering)
		{
			Send(m_stunServer, stun::BuildBindingRequest(m_discoveryId));
			return;
		}

		// Every candidate is checked on every retry rather than tried in turn: the peer
		// is punching at the same moment, and the pair only opens if both sides have sent
		// toward each other, so serialising the attempts halves the chance of overlap.
		for (const Check& check: m_checks)
		{
			Send(check.target, stun::BuildBindingRequest(check.id));
		}
	}

	std::optional<NatTraversal::Endpoint> NatTraversal::ParseEndpoint(const std::string& address, std::uint16_t port)
	{
		ENetAddress parsed{};
		if (enet_address_set_host_ip(&parsed, address.c_str()) != 0)
		{
			return std::nullopt;
		}
		return Endpoint{parsed.host, port};
	}

	std::string NatTraversal::FormatAddress(const Endpoint& endpoint)
	{
		ENetAddress address{};
		address.host = endpoint.host;
		address.port = endpoint.port;
		char text[64] = {};
		if (enet_address_get_host_ip(&address, text, sizeof(text)) != 0)
		{
			return {};
		}
		return text;
	}

	std::vector<NatTraversal::Endpoint> NatTraversal::LocalCandidates(std::uint16_t port)
	{
		std::vector<Endpoint> out;
		const auto add = [&out, port](std::uint32_t host) {
			if (host == 0)
			{
				return;
			}
			const Endpoint candidate{host, port};
			if (std::ranges::find(out, candidate) == out.end())
			{
				out.push_back(candidate);
			}
		};

		// The address the routing table would actually use to leave this machine.
		//
		// Asked this way rather than by enumerating adapters, because enumerating is
		// platform code - GetAdaptersAddresses here, getifaddrs there - and because a
		// machine with a VPN up, a virtual switch, and both Wi-Fi and Ethernet has
		// several answers of which only one is right. Connecting a UDP socket sends
		// nothing; it only asks the kernel which local address it WOULD send from, which
		// is precisely the question. The destination is documentation space, so even a
		// misread cannot aim anything at a real host.
		if (const ENetSocket probe = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM); probe != ENET_SOCKET_NULL)
		{
			ENetAddress routable{};
			if (enet_address_set_host_ip(&routable, "203.0.113.1") == 0)
			{
				routable.port = 9;
				ENetAddress local{};
				if (enet_socket_connect(probe, &routable) == 0 && enet_socket_get_address(probe, &local) == 0)
				{
					add(local.host);
				}
			}
			enet_socket_destroy(probe);
		}

		// The machine's own name, as a fallback and as a second opinion. On a host with
		// one interface this is the same answer; on one where the route probe failed -
		// no default route at all, which is a LAN with no internet - it is the only one.
		ENetAddress named{};
		if (enet_address_set_host(&named, "localhost") == 0)
		{
			add(named.host);
		}
		return out;
	}
} // namespace aether::net
