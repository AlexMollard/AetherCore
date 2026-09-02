#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "net/NatTraversal.hpp"
#include "net/NetworkSubsystem.hpp"

using namespace aether;
using Endpoint = net::NatTraversal::Endpoint;
using State = net::NatTraversal::State;

namespace
{
	// Runs both sides forward together. A punch only completes when each has sent toward
	// the other, so stepping one to completion before touching the other would never open
	// anything - the same reason it does not work to try candidates strictly in turn.
	void PumpBoth(net::NetworkSubsystem& a, net::NetworkSubsystem& b, int iterations = 200)
	{
		for (int i = 0; i < iterations; ++i)
		{
			if (a.Traversal()->GetState() == State::Open && b.Traversal()->GetState() == State::Open)
			{
				return;
			}
			a.Traversal()->Tick(0.05f);
			b.Traversal()->Tick(0.05f);
			a.Poll();
			b.Poll();
		}
	}
} // namespace

TEST_CASE("An address survives being written down and read back")
{
	// Candidates cross a signalling channel as text, and the two halves of ENet's
	// address convention disagree - host in network order, port in host order - so a
	// round trip is where a byte-order slip would surface.
	const auto endpoint = net::NatTraversal::ParseEndpoint("127.0.0.1", 24701);
	REQUIRE(endpoint.has_value());
	CHECK(endpoint->port == 24701);
	CHECK(net::NatTraversal::FormatAddress(*endpoint) == "127.0.0.1");

	const auto routable = net::NatTraversal::ParseEndpoint("203.0.113.7", 9);
	REQUIRE(routable.has_value());
	CHECK(net::NatTraversal::FormatAddress(*routable) == "203.0.113.7");

	// A host name is refused rather than resolved: a candidate is an address a NAT
	// already reported, so anything needing a lookup did not come from one.
	CHECK_FALSE(net::NatTraversal::ParseEndpoint("example.invalid", 80).has_value());
}

TEST_CASE("Traversal exists for a socket the transport owns, and dies with it")
{
	net::NetworkSubsystem host;
	CHECK(host.Traversal() == nullptr); // nothing to punch from yet

	REQUIRE(host.Host(24702, 4));
	REQUIRE(host.Traversal() != nullptr);
	CHECK(host.Traversal()->GetState() == State::Idle);

	// Tearing the transport down must take the intercept with it; a callback outliving
	// its owner fires on the next datagram that happens to arrive.
	host.Disconnect();
	CHECK(host.Traversal() == nullptr);
}

TEST_CASE("Two peers punch a path to each other over their own transport sockets")
{
	net::NetworkSubsystem a;
	net::NetworkSubsystem b;
	REQUIRE(a.Host(24703, 4));
	REQUIRE(b.Host(24704, 4));

	const auto toB = net::NatTraversal::ParseEndpoint("127.0.0.1", 24704);
	const auto toA = net::NatTraversal::ParseEndpoint("127.0.0.1", 24703);
	REQUIRE(toB.has_value());
	REQUIRE(toA.has_value());

	const std::vector<Endpoint> bCandidates{*toB};
	const std::vector<Endpoint> aCandidates{*toA};
	a.Traversal()->BeginPunch(bCandidates);
	b.Traversal()->BeginPunch(aCandidates);
	CHECK(a.Traversal()->GetState() == State::Punching);

	PumpBoth(a, b);

	CHECK(a.Traversal()->GetState() == State::Open);
	CHECK(b.Traversal()->GetState() == State::Open);

	// The endpoint that answered is what a connect must then be aimed at: the mapping
	// belongs to that exact pair, and any other address has no hole to travel through.
	const auto opened = a.Traversal()->OpenPath();
	REQUIRE(opened.has_value());
	CHECK(opened->port == 24704);
}

TEST_CASE("Punching at nowhere fails with a reason a player can act on")
{
	net::NetworkSubsystem a;
	REQUIRE(a.Host(24705, 4));

	// Discard, which answers nothing - standing in for the peer whose NAT gives every
	// destination a different mapping, so no check it sends can ever be answered.
	const auto nowhere = net::NatTraversal::ParseEndpoint("127.0.0.1", 9);
	REQUIRE(nowhere.has_value());
	const std::vector<Endpoint> candidates{*nowhere};
	a.Traversal()->BeginPunch(candidates);

	for (int i = 0; i < 300 && a.Traversal()->GetState() == State::Punching; ++i)
	{
		a.Traversal()->Tick(0.05f);
		a.Poll();
	}

	CHECK(a.Traversal()->GetState() == State::Failed);
	// Naming the cause matters: "connection failed" sends someone to their router for a
	// setting that cannot help, which is the wrong half of the day to lose.
	CHECK(a.Traversal()->FailureReason().find("relay") != std::string::npos);
}

TEST_CASE("Being asked to punch at nothing is refused rather than waited out")
{
	net::NetworkSubsystem a;
	REQUIRE(a.Host(24706, 4));

	a.Traversal()->BeginPunch({});
	CHECK(a.Traversal()->GetState() == State::Failed);
}

// Skipped by default: it needs the internet, and a test suite that fails when a
// third party's server is down is a test suite people learn to ignore. Run it on
// demand with --test-case="*real STUN*" --no-skip after touching StunMessage or the
// discovery path - everything else here proves the messages are well-formed by our own
// reading of RFC 5389, which is exactly the assumption a real server can falsify.
TEST_CASE("Discovery works against a real STUN server" * doctest::skip())
{
	net::NetworkSubsystem host;
	REQUIRE(host.Host(24707, 4));
	REQUIRE(host.Traversal()->BeginDiscovery("stun.l.google.com", 19302));

	// Real seconds, because a real round trip takes them.
	for (int i = 0; i < 250 && host.Traversal()->GetState() == State::Discovering; ++i)
	{
		host.Traversal()->Tick(0.02f);
		host.Poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	INFO("failure reason: " << host.Traversal()->FailureReason());
	REQUIRE(host.Traversal()->GetState() == State::Discovered);

	const auto reflexive = host.Traversal()->PublicEndpoint();
	REQUIRE(reflexive.has_value());
	MESSAGE("public endpoint: " << net::NatTraversal::FormatAddress(*reflexive) << ':' << reflexive->port);

	// A parse that silently produced zeroes would look like success otherwise.
	CHECK(reflexive->host != 0);
	CHECK(reflexive->port != 0);
}
