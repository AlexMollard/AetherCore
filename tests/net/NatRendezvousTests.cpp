#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "net/NatRendezvous.hpp"
#include "net/NatTraversal.hpp"
#include "net/NetworkSubsystem.hpp"
#include "net/Signaling.hpp"

using namespace aether;
using State = net::NatTraversal::State;

namespace
{
	// Offers one endpoint nobody has offered before, on every Poll, forever - the
	// shape of a hostile peer (or anyone who has learned the room code) whose goal
	// is to keep the punch window re-arming and to aim a connectivity check at each
	// fresh address it invents. Ports are loopback and dark, so nothing answers and
	// nothing leaves the machine.
	class FloodChannel final : public net::ISignalingChannel
	{
	public:
		void Publish(const net::CandidateSet& /*candidates*/) override {}

		std::optional<net::CandidateSet> Poll() override
		{
			net::CandidateSet set;
			const auto endpoint = net::NatTraversal::ParseEndpoint("127.0.0.1", static_cast<std::uint16_t>(20000 + m_next));
			++m_next;
			if (endpoint.has_value())
			{
				set.endpoints.push_back(*endpoint);
			}
			return set;
		}

	private:
		int m_next = 0;
	};
} // namespace

TEST_CASE("A flood of fresh candidates stops at the cap and cannot hold the punch window open")
{
	net::NetworkSubsystem transport;
	REQUIRE(transport.Host(24790, 4));

	FloodChannel channel;
	net::NatRendezvous rendezvous(*transport.Traversal(), channel, 24790);
	rendezvous.Begin(""); // no STUN: nothing external is asked, and no path ever opens

	// Thirty simulated seconds - past the punch's own ten-second budget and the
	// rendezvous's twenty-second peer budget - while a fresh candidate arrives
	// every single frame. The bound keeps a hostile flood from turning this into
	// an infinite loop rather than a slow one.
	bool failed = false;
	for (int i = 0; i < 600; ++i)
	{
		rendezvous.Tick(0.05f);
		transport.Poll();
		if (rendezvous.GetState() == State::Failed)
		{
			failed = true;
			break;
		}
	}

	// Once the accumulator is at kMaxCandidates no candidate is ever new again, so
	// nothing re-arms the punch and its own timeout lands. Pre-fix, every fresh
	// endpoint grew the list and restarted the window: this stays Punching forever.
	CHECK(failed);

	// And the flood armed no more checks than the cap allows. Pre-fix this was one
	// per flood datagram - 600 here, unbounded in the wild.
	CHECK(transport.Traversal()->PeerCandidates().size() == net::kMaxCandidates);
}

TEST_CASE("A pinned candidate rides along on the first publish")
{
	// The shape of a router mapping's external endpoint (NetTraversalSession's
	// Mapped rung): one more address this socket is reachable on. It has to be in
	// what the peer is handed, or the mapping that was asked for buys nothing.
	net::NetworkSubsystem transport;
	REQUIRE(transport.Host(24791, 4));

	net::LocalSignalingChannel ours;
	net::LocalSignalingChannel theirs;
	net::LocalSignalingChannel::Pair(ours, theirs);

	const auto mapped = net::NatTraversal::ParseEndpoint("127.0.0.1", 41234);
	REQUIRE(mapped.has_value());

	net::NatRendezvous rendezvous(*transport.Traversal(), ours, 24791, mapped);
	rendezvous.Begin("");

	const auto offered = theirs.Poll();
	REQUIRE(offered.has_value());
	CHECK(std::ranges::find(offered->endpoints, *mapped) != offered->endpoints.end());
}
