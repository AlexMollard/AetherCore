#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "net/ServerConnection.hpp"
#include "scripting/SceneContext.hpp"
#include "scene/World.hpp"

extern "C" std::uint64_t aether_server_connection_create();
extern "C" void aether_server_connection_release(std::uint64_t handle);

using aether::net::ServerConnections;

TEST_CASE("Server connections validate a browser port override before selecting the native endpoint")
{
	CHECK(ServerConnections::ResolveServerPort(27050, 0) == 27050);
	CHECK(ServerConnections::ResolveServerPort(27050, 27056) == 27056);
	CHECK(ServerConnections::ResolveServerPort(0, 65535) == 65535);
	CHECK(ServerConnections::ResolveServerPort(0, 0) == 0);
	CHECK(ServerConnections::ResolveServerPort(65536, 0) == 0);
	CHECK(ServerConnections::ResolveServerPort(27050, -1) == 0);
	CHECK(ServerConnections::ResolveServerPort(27050, 65536) == 0);
}

namespace
{
	bool Pump(aether::net::NetworkSubsystem& host, ServerConnections& connections, std::uint64_t handle, auto done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (std::chrono::steady_clock::now() < deadline)
		{
			host.Poll();
			connections.Poll(handle);
			if (done())
			{
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return false;
	}

	bool ConnectBoth(aether::net::NetworkSubsystem& host, ServerConnections& connections, std::uint64_t handle)
	{
		bool hostConnected = false;
		return Pump(host, connections, handle, [&]
		{
			for (const auto& event : host.Events())
			{
				hostConnected |= event.kind == aether::net::NetEvent::Kind::Connected;
			}
			return hostConnected && connections.IsConnected(handle);
		});
	}
}

TEST_CASE("Server connections release only the removed script instance on a shared entity")
{
	ServerConnections connections;
	const auto firstOwner = ServerConnections::OwnerKey(42, 0);
	const auto secondOwner = ServerConnections::OwnerKey(42, 1);
	const auto first = connections.Create(firstOwner);
	const auto second = connections.Create(secondOwner);
	connections.ReleaseOwner(firstOwner);
	CHECK_FALSE(connections.Connect(first, "127.0.0.1", 27495));
	CHECK(connections.Connect(second, "127.0.0.1", 27495));
}

TEST_CASE("Server connections reject creation outside live script callbacks and during teardown")
{
	using namespace aether::app::scripting;
	aether::World world;
	SceneContext context;
	context.world = &world;
	ActiveContextScope sceneScope(context);
	CHECK(aether_server_connection_create() == 0);
	const auto actor = world.Create();
	ActiveScriptEntityScope actorScope(context, actor);
	CHECK(aether_server_connection_create() == 0);
	ServerConnectionOwnerScope ownerScope(context, ServerConnections::OwnerKey(actor.id, 0));
	const auto handle = aether_server_connection_create();
	REQUIRE(handle != 0);
	context.serverConnectionsStopping = true;
	CHECK(aether_server_connection_create() == 0);
	context.serverConnections->Clear();
	aether_server_connection_release(handle);
	CHECK_FALSE(context.serverConnections->Connect(handle, "127.0.0.1", 27495));
	context.serverConnectionsStopping = false;
	world.Destroy(actor);
	CHECK(aether_server_connection_create() == 0);
}

TEST_CASE("Server connections bound handles and reject disposed and foreign handles")
{
	ServerConnections connections;
	const auto first = connections.Create(42);
	REQUIRE(first != 0);
	CHECK_FALSE(connections.Send(first, "hello", true));
	CHECK_FALSE(connections.LastError(first).empty());
	for (std::size_t i = 1; i < ServerConnections::kMaxConnections; ++i)
	{
		REQUIRE(connections.Create(43) != 0);
	}
	CHECK(connections.Create() == 0);
	connections.ReleaseOwner(42);
	CHECK_FALSE(connections.Connect(first, "127.0.0.1", 27491));
	const auto replacement = connections.Create();
	REQUIRE(replacement != 0);
	CHECK(replacement != first);
	ServerConnections other;
	const auto foreign = other.Create();
	CHECK_FALSE(connections.Connect(foreign, "127.0.0.1", 27491));
	connections.Clear();
	CHECK_FALSE(connections.IsConnected(replacement));
	CHECK_FALSE(connections.Connect(replacement, "127.0.0.1", 27491));
}

TEST_CASE("Server connections exchange copied UTF8 without consuming undersized reads")
{
	aether::net::NetworkSubsystem host;
	REQUIRE(host.Host(27491, 2));
	ServerConnections connections;
	const auto handle = connections.Create();
	REQUIRE(connections.Connect(handle, "127.0.0.1", 27491));
	REQUIRE(ConnectBoth(host, connections, handle));
	const std::string text = "caf\xc3\xa9 \xf0\x9f\x8c\xb3";
	host.Broadcast(0, true, std::as_bytes(std::span(text)));
	REQUIRE(Pump(host, connections, handle, [&] { return connections.Receive(handle, nullptr, 0) > 0; }));
	CHECK(connections.Receive(handle, nullptr, 0) == static_cast<int>(text.size()));
	char small[2]{};
	CHECK(connections.Receive(handle, small, 2) == -1);
	std::string received(text.size(), '\0');
	CHECK(connections.Receive(handle, received.data(), static_cast<int>(received.size())) == static_cast<int>(text.size()));
	CHECK(received == text);
	CHECK(connections.Receive(handle, nullptr, 0) == 0);
	CHECK(connections.Send(handle, text, true));
	bool echoed = false;
	REQUIRE(Pump(host, connections, handle, [&]
	{
		for (const auto& event : host.Events())
		{
			if (event.kind == aether::net::NetEvent::Kind::Data)
			{
				echoed = std::string(reinterpret_cast<const char*>(event.data.data()), event.data.size()) == text;
			}
		}
		return echoed;
	}));
	CHECK_FALSE(connections.Send(handle, std::string(ServerConnections::kMaxMessageBytes + 1, 'x'), true));
	CHECK_FALSE(connections.Send(handle, std::string("\xc0\xaf", 2), true));
	CHECK_FALSE(connections.Send(handle, std::string("a\0b", 3), true));
	connections.Disconnect(handle);
	CHECK_FALSE(connections.IsConnected(handle));
	CHECK(connections.Receive(handle, nullptr, 0) == 0);
}

TEST_CASE("Server connections isolate sockets and bound sends between polls")
{
	aether::net::NetworkSubsystem host;
	REQUIRE(host.Host(27494, 2));
	ServerConnections connections;
	const auto first = connections.Create(1);
	const auto second = connections.Create(2);
	REQUIRE(connections.Connect(first, "127.0.0.1", 27494));
	REQUIRE(connections.Connect(second, "127.0.0.1", 27494));
	REQUIRE(Pump(host, connections, first, [&]
	{
		connections.Poll(second);
		return connections.IsConnected(first) && connections.IsConnected(second);
	}));
	connections.Disconnect(first);
	CHECK(connections.IsConnected(second));
	for (std::size_t i = 0; i < ServerConnections::kMaxQueuedMessages; ++i)
	{
		REQUIRE(connections.Send(second, "message", true));
	}
	CHECK_FALSE(connections.Send(second, "overflow", true));
	CHECK(connections.LastError(second).find("queue") != std::string::npos);
	connections.ReleaseOwner(1);
	CHECK(connections.IsConnected(second));
	connections.Clear();
	CHECK_FALSE(connections.Send(second, "stale", true));
}

TEST_CASE("Server connections disconnect a peer that overflows the receive queue")
{
	aether::net::NetworkSubsystem host;
	REQUIRE(host.Host(27492, 1));
	ServerConnections connections;
	const auto handle = connections.Create();
	REQUIRE(connections.Connect(handle, "127.0.0.1", 27492));
	REQUIRE(ConnectBoth(host, connections, handle));
	for (std::size_t i = 0; i <= ServerConnections::kMaxQueuedMessages; ++i)
	{
		const std::string message = "message";
		host.Broadcast(0, true, std::as_bytes(std::span(message)));
	}
	REQUIRE(Pump(host, connections, handle, [&] { return !connections.IsConnected(handle); }));
	CHECK(connections.LastError(handle).find("queue") != std::string::npos);
	CHECK(connections.Receive(handle, nullptr, 0) == 0);
}

TEST_CASE("Server connections reject malformed incoming UTF8")
{
	aether::net::NetworkSubsystem host;
	REQUIRE(host.Host(27493, 1));
	ServerConnections connections;
	const auto handle = connections.Create();
	REQUIRE(connections.Connect(handle, "127.0.0.1", 27493));
	REQUIRE(ConnectBoth(host, connections, handle));
	const std::string malformed("\xed\xa0\x80", 3);
	host.Broadcast(0, true, std::as_bytes(std::span(malformed)));
	REQUIRE(Pump(host, connections, handle, [&] { return !connections.IsConnected(handle); }));
	CHECK(connections.LastError(handle).find("UTF-8") != std::string::npos);
}
