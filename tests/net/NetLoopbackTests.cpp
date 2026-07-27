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

TEST_CASE("Two subsystems can coexist and outlive each other")
{
	// This is the scenario the shared ENet refcount (EnetInit.hpp) exists for: an
	// editor ControlServer and a game NetworkSubsystem sharing one process, where
	// either can tear down while the other is still active. It will NOT fail against
	// a broken (non-shared, or unrefcounted) implementation on Windows, because the
	// underlying WSAStartup/timeBeginPeriod primitives tolerate redundant init/deinit
	// - so passing here is not proof the refcount is genuine. It pins the intended
	// usage and would catch a regression on a platform where double-deinit is fatal.
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24682, 4));
	REQUIRE(client.Connect("127.0.0.1", 24682));

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

	// The client tears down and releases its ENet reference while the host is
	// still active - the host must keep working afterwards.
	client.Disconnect();

	const std::string payload = "still alive";
	host.Send(hostSawPeer, net::kChannelReliable, true,
	        std::as_bytes(std::span<const char>{payload.data(), payload.size()}));
	host.Poll();
	CHECK(host.Role() == net::NetRole::Host);

	host.Disconnect();
	CHECK(host.Role() == net::NetRole::Offline);
}

TEST_CASE("A send on an invalid channel is a no-op")
{
	net::NetworkSubsystem host;
	net::NetworkSubsystem client;

	REQUIRE(host.Host(24683, 4));
	REQUIRE(client.Connect("127.0.0.1", 24683));

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

	const std::string payload = "should not arrive";
	host.Send(hostSawPeer, -1, true, std::as_bytes(std::span<const char>{payload.data(), payload.size()}));
	host.Send(hostSawPeer, net::kChannelCount, true, std::as_bytes(std::span<const char>{payload.data(), payload.size()}));

	bool gotData = false;
	PumpUntil(host, client,
	        [&]
	        {
		        for (const net::NetEvent& e: client.Events())
		        {
			        if (e.kind == net::NetEvent::Kind::Data)
			        {
				        gotData = true;
			        }
		        }
		        return false;
	        },
	        200);
	CHECK_FALSE(gotData);

	client.Disconnect();
	host.Disconnect();
}
