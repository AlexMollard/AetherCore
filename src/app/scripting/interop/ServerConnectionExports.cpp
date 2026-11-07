#include "scripting/interop/InteropCommon.hpp"

#include <cstring>

#include "net/ServerConnection.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	aether::net::ServerConnections* Connections(bool create = false)
	{
		if (g_activeContext == nullptr)
		{
			return nullptr;
		}
		if (create && !g_activeContext->serverConnections)
		{
			g_activeContext->serverConnections = std::make_shared<aether::net::ServerConnections>();
		}
		return g_activeContext->serverConnections.get();
	}

	std::string_view BoundedString(const char* value, std::size_t limit)
	{
		if (value == nullptr)
		{
			return {};
		}
		std::size_t size = 0;
		while (size <= limit && value[size] != '\0')
		{
			++size;
		}
		return {value, size};
	}
}

AE_SCRIPT_API std::uint64_t aether_server_connection_create()
{
	return SafeExport([]() -> std::uint64_t
	{
		if (g_activeContext == nullptr || g_activeContext->serverConnectionsStopping || g_activeContext->activeServerConnectionOwner == 0 ||
			g_activeContext->world == nullptr || !g_activeContext->activeScriptEntity.IsValid() ||
			static_cast<std::uint32_t>(g_activeContext->activeServerConnectionOwner >> 32) != g_activeContext->activeScriptEntity.id ||
			!g_activeContext->world->GetRegistry().valid(aether::World::ToEntt(g_activeContext->activeScriptEntity)))
		{
			return 0;
		}
		auto* connections = Connections(true);
		return connections ? connections->Create(g_activeContext->activeServerConnectionOwner) : 0;
	});
}

AE_SCRIPT_API void aether_server_connection_release(std::uint64_t handle)
{
	SafeExport([&]
	{
		if (auto* connections = Connections())
		{
			connections->Release(handle);
		}
	});
}

AE_SCRIPT_API std::int32_t aether_server_connection_connect(std::uint64_t handle, const char* host, std::int32_t port, const char* gatewayUrl, const char* gatewayToken, const char* targetAlias)
{
	return SafeExport([&]() -> std::int32_t
	{
		auto* connections = Connections();
		return connections && connections->Connect(handle, BoundedString(host, 253), port, BoundedString(gatewayUrl, 2048), BoundedString(gatewayToken, 4096), BoundedString(targetAlias, 128)) ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_server_connection_poll(std::uint64_t handle)
{
	SafeExport([&]
	{
		if (auto* connections = Connections())
		{
			connections->Poll(handle);
		}
	});
}

AE_SCRIPT_API std::int32_t aether_server_connection_send(std::uint64_t handle, const char* message, std::int32_t length, std::int32_t reliable)
{
	return SafeExport([&]() -> std::int32_t
	{
		auto* connections = Connections();
		if (connections == nullptr || message == nullptr || length <= 0 || length > static_cast<std::int32_t>(aether::net::ServerConnections::kMaxMessageBytes))
		{
			return 0;
		}
		return connections->Send(handle, {message, static_cast<std::size_t>(length)}, reliable != 0) ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_server_connection_receive(std::uint64_t handle, char* buffer, std::int32_t capacity)
{
	return SafeExport([&]() -> std::int32_t
	{
		auto* connections = Connections();
		return connections ? connections->Receive(handle, buffer, capacity) : 0;
	});
}

AE_SCRIPT_API void aether_server_connection_disconnect(std::uint64_t handle)
{
	SafeExport([&]
	{
		if (auto* connections = Connections())
		{
			connections->Disconnect(handle);
		}
	});
}

AE_SCRIPT_API std::int32_t aether_server_connection_is_connected(std::uint64_t handle)
{
	return SafeExport([&]() -> std::int32_t
	{
		auto* connections = Connections();
		return connections && connections->IsConnected(handle) ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_server_connection_last_error(std::uint64_t handle, char* buffer, std::int32_t capacity)
{
	return SafeExport([&]() -> std::int32_t
	{
		auto* connections = Connections();
		const std::string error = connections ? connections->LastError(handle) : "Server connection scene is no longer active";
		if (buffer == nullptr)
		{
			return static_cast<std::int32_t>(error.size());
		}
		if (capacity < 0 || static_cast<std::size_t>(capacity) < error.size())
		{
			return -1;
		}
		std::memcpy(buffer, error.data(), error.size());
		return static_cast<std::int32_t>(error.size());
	});
}
