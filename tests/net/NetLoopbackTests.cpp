#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "net/NetworkSubsystem.hpp"

using namespace aether;

namespace
{
	// Pump both ends until `done` or the deadline. ENet needs several service calls
	// to complete a handshake, so a single Poll() is never enough.
	bool PumpUntil(net::NetworkSubsystem& a, net::NetworkSubsystem& b, auto done, int maxMs = 2000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			a.Poll();
			b.Poll();
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}
} // namespace

TEST_CASE("A client connects to a host over loopback and exchanges a payload")
{
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24681, 4));
	CHECK(host.Role() == net::NetRole::Host);

	REQUIRE(client.Connect("127.0.0.1", 24681));

	net::ConnectionId hostSawPeer = 0;
	const bool connected = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: host.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Connected)
			        {
				        hostSawPeer = e.peer;
			        }
		        }
		        return hostSawPeer != 0;
	        });
	REQUIRE(connected);

	const std::string payload = "hello";
	host.Send(hostSawPeer, net::kChannelReliable, true,
	        std::as_bytes(std::span<const char>{payload.data(), payload.size()}));

	std::string received;
	const bool gotData = PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: client.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Data)
			        {
				        received.assign(reinterpret_cast<const char*>(e.data.data()), e.data.size());
			        }
		        }
		        return !received.empty();
	        });
	REQUIRE(gotData);
	CHECK(received == "hello");

	client.Disconnect();
	host.Disconnect();
	CHECK(host.Role() == net::NetRole::Offline);
}
