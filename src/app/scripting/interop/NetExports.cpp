#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// The Net.* script API's native half.
//
// EVERY export here is offline-safe. A title screen asks Net.IsHost before anything
// has connected, and a single-player build never registers a NetworkContext at all,
// so a missing context is a normal state that returns false/0/empty - never a crash
// and never a log line.
namespace
{
	aether::net::NetworkContext* Context()
	{
		const auto& ctx = ActiveContext();
		if (ctx.services == nullptr)
		{
			return nullptr;
		}
		return ctx.services->TryGet<aether::net::NetworkContext>();
	}

	// Shared with Ui's string getters: write into the caller's buffer and return the
	// byte count, so no allocation crosses the managed boundary.
	std::int32_t CopyOut(const std::string& value, char* buffer, std::int32_t capacity)
	{
		if (buffer == nullptr || capacity <= 0 || value.empty())
		{
			return 0;
		}
		auto n = std::min<std::size_t>(static_cast<std::size_t>(capacity), value.size());
		// Back off to a UTF-8 code-point boundary: a byte with the high bits
		// `10xxxxxx` is a continuation byte, never the first byte of a character, so
		// cutting there would split a multi-byte character and hand the managed side
		// a dangling continuation sequence that decodes to a replacement character.
		while (n > 0 && (static_cast<unsigned char>(value[n]) & 0xC0u) == 0x80u)
		{
			--n;
		}
		std::memcpy(buffer, value.data(), n);
		return static_cast<std::int32_t>(n);
	}
} // namespace

AE_SCRIPT_API std::int32_t aether_net_host(std::uint16_t port, std::int32_t maxConnections)
{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 0;
	}
	return context->StartHost(ActiveWorld(), port, maxConnections) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_net_connect(const char* hostUtf8, std::uint16_t port)
{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr || hostUtf8 == nullptr)
	{
		return 0;
	}
	return context->StartClient(ActiveWorld(), hostUtf8, port) ? 1 : 0;
}

AE_SCRIPT_API void aether_net_disconnect()
{
	if (aether::net::NetworkContext* context = Context())
	{
		context->Stop(ActiveWorld());
	}
}

AE_SCRIPT_API std::int32_t aether_net_is_host()
{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr && context->IsHost() ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_net_is_client()
{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr && context->IsClient() ? 1 : 0;
}

AE_SCRIPT_API void aether_net_set_replication_ready(std::int32_t ready)
{
	// Offline, this is simply nothing: there is no session to hold anything back from,
	// and a menu that declares itself not-ready before a connect it never makes must not
	// be left in a state a later single-player session can see.
	if (aether::net::NetworkContext* context = Context())
	{
		context->SetReplicationReady(ActiveWorld(), ready != 0);
	}
}

AE_SCRIPT_API std::int32_t aether_net_is_replication_ready()
{
	const aether::net::NetworkContext* context = Context();
	// No networking in this build: this peer is standing in the only world there is.
	return context == nullptr || context->IsReplicationReady() ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_net_is_connected()
{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr && context->IsConnected() ? 1 : 0;
}

AE_SCRIPT_API std::uint32_t aether_net_local_connection_id()
{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? context->LocalConnectionId() : 0u;
}

AE_SCRIPT_API std::int32_t aether_net_connections(std::uint32_t* buffer, std::int32_t capacity)
{
	// Host-only by construction: NetSession only ever records a connection on the
	// host (NetworkReceiveSystem::OnConnected), so a client and an offline build both
	// report an empty list rather than a special case here.
	aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 0;
	}
	const std::vector<aether::net::ConnectionId>& connections = context->Session().Connections();
	const auto total = static_cast<std::int32_t>(connections.size());
	if (buffer == nullptr || capacity <= 0)
	{
		// Size query: the managed side asks for the count first, then asks again with
		// a buffer, so no allocation crosses the boundary and neither side guesses a
		// maximum peer count.
		return total;
	}
	const std::int32_t written = std::min(total, capacity);
	std::memcpy(buffer, connections.data(), static_cast<std::size_t>(written) * sizeof(std::uint32_t));
	return written;
}

AE_SCRIPT_API std::uint32_t aether_net_spawn(const char* prefabUtf8, Vec3 position, std::uint32_t owner)
{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr || prefabUtf8 == nullptr)
	{
		return 0;
	}
	return context->SpawnPrefab(ActiveWorld(), prefabUtf8, ToGlm(position), owner).id;
}

AE_SCRIPT_API void aether_net_despawn(std::uint32_t entityId)
{
	aether::net::NetworkContext* context = Context();
	const aether::Entity entity{entityId};
	if (!entity.IsValid())
	{
		return;
	}
	if (context == nullptr)
	{
		// Offline, Net.Despawn still has to mean "this entity goes away", or a script
		// written for both modes leaks entities in single-player. A bare Destroy
		// would only remove the root and strand every child - see the identical
		// note on ecs::DestroyHierarchy in ControlMethods.cpp.
		aether::ecs::DestroyHierarchy(ActiveWorld(), entity);
		return;
	}
	context->Despawn(ActiveWorld(), entity);
}

AE_SCRIPT_API std::int32_t aether_net_has_authority(std::uint32_t entityId)
{
	const aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 1; // offline: local state is the only state
	}
	return context->HasAuthority(ActiveWorld(), aether::Entity{entityId}) ? 1 : 0;
}

AE_SCRIPT_API std::int32_t aether_net_is_owner(std::uint32_t entityId)
{
	const aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 1;
	}
	return context->IsOwner(ActiveWorld(), aether::Entity{entityId}) ? 1 : 0;
}

AE_SCRIPT_API void aether_net_set_player_name(std::uint32_t entityId, const char* nameUtf8)
{
	// Deliberately independent of any session: NetPlayer is a plain component, so a
	// name set on the menu survives into the session that replicates it later.
	const aether::Entity entity{entityId};
	if (!entity.IsValid() || nameUtf8 == nullptr)
	{
		return;
	}
	auto& world = ActiveWorld();
	// A display name is REPLICATED STATE, and the owner of an entity is authoritative
	// for its state. A peer writing this on somebody else's player is writing a value
	// that player's owner overwrites on its next send, so whether the write survives is
	// a race - which is exactly how "some players' names do not show" happened. Refused
	// here, in the framework, so no project can reintroduce it by accident.
	//
	// Nothing legitimate is lost: an entity with no NetworkIdentity (a menu carrier),
	// the host's own player, and every entity in an offline game all report owned.
	const aether::net::NetworkContext* context = Context();
	if (context != nullptr && !context->IsOwner(world, entity))
	{
		AE_WARN(aether::LogCategory::App,
		        "Net.SetPlayerName: refused on entity {} - this peer does not own it. The owner sets its own name.",
		        entityId);
		return;
	}
	if (auto* player = world.TryGet<aether::net::NetPlayer>(entity))
	{
		player->displayName = nameUtf8;
		return;
	}
	world.Emplace<aether::net::NetPlayer>(entity, aether::net::NetPlayer{.displayName = nameUtf8});
}

AE_SCRIPT_API std::int32_t aether_net_claim_player_name(std::uint32_t entityId, const char* desiredUtf8, char* buffer,
        std::int32_t capacity)
{
	const aether::Entity entity{entityId};
	if (!entity.IsValid() || desiredUtf8 == nullptr)
	{
		return 0;
	}
	aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		// No networking in this build: there is nobody to collide with, so claiming a
		// name is setting it. Offline parity is a hard rule for every Net.* export.
		aether_net_set_player_name(entityId, desiredUtf8);
		return CopyOut(desiredUtf8, buffer, capacity);
	}
	return CopyOut(context->ClaimPlayerName(ActiveWorld(), entity, desiredUtf8), buffer, capacity);
}

AE_SCRIPT_API std::int32_t aether_net_disconnect_reason(char* buffer, std::int32_t capacity)
{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? CopyOut(context->DisconnectReason(), buffer, capacity) : 0;
}

AE_SCRIPT_API std::int32_t aether_net_get_player_name(std::uint32_t entityId, char* buffer, std::int32_t capacity)
{
	const auto* player = ActiveWorld().TryGet<aether::net::NetPlayer>(aether::Entity{entityId});
	return player != nullptr ? CopyOut(player->displayName, buffer, capacity) : 0;
}

AE_SCRIPT_API std::int32_t aether_net_last_error(char* buffer, std::int32_t capacity)
{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? CopyOut(context->LastError(), buffer, capacity) : 0;
}
