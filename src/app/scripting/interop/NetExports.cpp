#include "scripting/interop/InteropCommon.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <entt/entt.hpp>

#include "net/NetComponents.hpp"
#include "net/NetworkContext.hpp"
#include "net/RoomCode.hpp"
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

	// Queue `entity` and everything under it for destruction at the end of the script
	// update, children first, exactly the order ecs::DestroyHierarchy uses.
	//
	// WHY DEFERRED. Net.Despawn is called from a script, and a script runs inside the
	// script runner's own walk of ScriptComponent storage - destroying an entity there
	// frees the component the loop is holding a pointer into. That is precisely why
	// Entity.Destroy defers (see aether_entity_destroy), and a second destruction path
	// that does not defer would make the same hazard reachable through the only call a
	// project has for removing a REPLICATED entity. The wire half is not deferred with
	// it: see NetworkContext::ReleaseForDespawn.
	void QueueHierarchyDestroy(aether::World& world, aether::Entity entity, std::vector<aether::Entity>& out)
	{
		if (const auto* hierarchy = world.TryGet<aether::HierarchyComponent>(entity))
		{
			// Copied: the recursion mutates the parent link of each child below.
			const std::vector<aether::Entity> children = hierarchy->children;
			for (const aether::Entity child: children)
			{
				QueueHierarchyDestroy(world, child, out);
			}
		}
		// Now rather than at flush time, so the surviving parent's child list stops
		// naming a doomed entity for the rest of this frame.
		aether::ecs::DetachFromParent(world, entity);
		out.push_back(entity);
	}
} // namespace

AE_SCRIPT_API std::int32_t aether_net_host(std::uint16_t port, std::int32_t maxConnections)
{
	return SafeExport([&] -> std::int32_t
	{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 0;
	}
	return context->StartHost(ActiveWorld(), port, maxConnections) ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_connect(const char* hostUtf8, std::uint16_t port)
{
	return SafeExport([&] -> std::int32_t
	{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr || hostUtf8 == nullptr)
	{
		return 0;
	}
	return context->StartClient(ActiveWorld(), hostUtf8, port) ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_net_disconnect()
{
	SafeExport([&] -> void
	{
	if (aether::net::NetworkContext* context = Context())
	{
		context->Stop(ActiveWorld());
	}
	});
}

AE_SCRIPT_API std::int32_t aether_net_is_host()
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr && context->IsHost() ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_is_client()
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr && context->IsClient() ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_net_set_replication_ready(std::int32_t ready)
{
	SafeExport([&] -> void
	{
	// Offline, this is simply nothing: there is no session to hold anything back from,
	// and a menu that declares itself not-ready before a connect it never makes must not
	// be left in a state a later single-player session can see.
	if (aether::net::NetworkContext* context = Context())
	{
		context->SetReplicationReady(ActiveWorld(), ready != 0);
	}
	});
}

AE_SCRIPT_API std::int32_t aether_net_is_replication_ready()
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	// No networking in this build: this peer is standing in the only world there is.
	return context == nullptr || context->IsReplicationReady() ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_is_connected()
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr && context->IsConnected() ? 1 : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_net_local_connection_id()
{
	return SafeExport([&] -> std::uint32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? context->LocalConnectionId() : 0u;
	});
}

AE_SCRIPT_API std::int32_t aether_net_connections(std::uint32_t* buffer, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
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
	});
}

AE_SCRIPT_API std::uint32_t aether_net_spawn(const char* prefabUtf8, Vec3 position, std::uint32_t owner)
{
	return SafeExport([&] -> std::uint32_t
	{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr || prefabUtf8 == nullptr)
	{
		return 0;
	}
	return context->SpawnPrefab(ActiveWorld(), prefabUtf8, ToGlm(position), owner).id;
	});
}

AE_SCRIPT_API void aether_net_despawn(std::uint32_t entityId)
{
	SafeExport([&] -> void
	{
	aether::net::NetworkContext* context = Context();
	const aether::Entity entity{entityId};
	auto& world = ActiveWorld();
	if (!entity.IsValid() || !world.GetRegistry().valid(aether::World::ToEntt(entity)))
	{
		return;
	}
	// No networking in this build: Net.Despawn still has to mean "this entity goes
	// away", or a script written for both modes leaks entities in single-player. The
	// whole subtree goes, not just the root - see QueueHierarchyDestroy.
	if (context != nullptr && !context->ReleaseForDespawn(world, entity))
	{
		return; // refused (a client naming an entity it does not own)
	}
	QueueHierarchyDestroy(world, entity, aether::app::scripting::ActiveContext().pendingDestroys);
	});
}

AE_SCRIPT_API std::int32_t aether_net_has_authority(std::uint32_t entityId)
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 1; // offline: local state is the only state
	}
	return context->HasAuthority(ActiveWorld(), aether::Entity{entityId}) ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_is_owner(std::uint32_t entityId)
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	if (context == nullptr)
	{
		return 1;
	}
	return context->IsOwner(ActiveWorld(), aether::Entity{entityId}) ? 1 : 0;
	});
}

AE_SCRIPT_API void aether_net_set_player_name(std::uint32_t entityId, const char* nameUtf8)
{
	SafeExport([&] -> void
	{
	// Deliberately independent of any session: NetPlayer is a plain component, so a
	// name set on the menu survives into the session that replicates it later.
	const aether::Entity entity{entityId};
	// EntityAlive, not just nonzero: the fallback below emplaces a NetPlayer, which
	// must not attach to a fabricated or already destroyed id.
	if (!entity.IsValid() || nameUtf8 == nullptr || !EntityAlive(entityId))
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
	});
}

AE_SCRIPT_API std::int32_t aether_net_claim_player_name(std::uint32_t entityId, const char* desiredUtf8, char* buffer,
        std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
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
	});
}

AE_SCRIPT_API std::int32_t aether_net_disconnect_reason(char* buffer, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? CopyOut(context->DisconnectReason(), buffer, capacity) : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_get_player_name(std::uint32_t entityId, char* buffer, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
	const auto* player = ActiveWorld().TryGet<aether::net::NetPlayer>(aether::Entity{entityId});
	return player != nullptr ? CopyOut(player->displayName, buffer, capacity) : 0;
	});
}

AE_SCRIPT_API std::uint32_t aether_net_get_player_ping(std::uint32_t entityId)
{
	return SafeExport([&] -> std::uint32_t
	{
	// Read straight off the component, not off the transport: on every peer but this
	// player's owner there is no link to that player to measure, and the replicated
	// value is the only answer that exists. On the owner it is the same number the
	// transport reported a tick ago, so one accessor serves both and a game never has
	// to ask which peer it is running on.
	const auto* player = ActiveWorld().TryGet<aether::net::NetPlayer>(aether::Entity{entityId});
	return player != nullptr ? player->pingMs : 0u;
	});
}

AE_SCRIPT_API std::uint32_t aether_net_round_trip_ms()
{
	return SafeExport([&] -> std::uint32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? context->LocalRoundTripMs() : 0u;
	});
}

AE_SCRIPT_API std::uint32_t aether_net_owner_of(std::uint32_t entityId)
{
	return SafeExport([&] -> std::uint32_t
	{
	const auto* identity = ActiveWorld().TryGet<aether::net::NetworkIdentity>(aether::Entity{entityId});
	if (identity != nullptr)
	{
		// Verbatim, INCLUDING kInvalidConnection - which is 0, which is also the host's
		// connection id, which is exactly what a host-owned entity's owner field holds.
		// Substituting "whoever is asking" for it here made every host-owned player read
		// as belonging to the client looking at it, so a roster keyed on this put the
		// host in the wrong place and stopped labelling it as the host at all.
		return identity->owner;
	}
	// Not replicated: a menu carrier, a HUD element, anything local. There is nobody else
	// it could belong to, and offline that is the answer for everything.
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? context->LocalConnectionId() : 0u;
	});
}

AE_SCRIPT_API std::int32_t aether_net_players(std::uint32_t* buffer, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
	// NetPlayer is the framework's own "this entity is a person in the session" mark,
	// so this is the roster with no game-side bookkeeping and no second copy of it. It
	// is answered from the WORLD rather than from the session, which is what makes it
	// give the same answer on a client - a client is told nothing about connections,
	// but it is holding every replicated player it can see.
	auto& world = ActiveWorld();
	std::int32_t total = 0;
	world.View<aether::net::NetPlayer>().each(
	        [&](entt::entity ent, aether::net::NetPlayer&)
	        {
		        if (buffer != nullptr && total < capacity)
		        {
			        buffer[total] = aether::World::FromEntt(ent).id;
		        }
		        ++total;
	        });
	if (buffer == nullptr || capacity <= 0)
	{
		return total; // size query, matching aether_net_connections
	}
	return std::min(total, capacity);
	});
}

AE_SCRIPT_API std::int32_t aether_net_last_error(char* buffer, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? CopyOut(context->LastError(), buffer, capacity) : 0;
	});
}

// ── Networking: NAT traversal ─────────────────────────────────────────────────
// Session-free by the same rule as everything above: a title screen calls
// TraversalState before any Host/Join has been attempted, and that has to read
// Idle rather than crash or warn.

AE_SCRIPT_API std::int32_t aether_net_configure_signaling(std::int32_t backend, const char* addressUtf8)
{
	return SafeExport([&] -> std::int32_t
	{
	aether::net::NetworkContext* context = Context();
	// Backend is a wire-adjacent value chosen by a script, not by the network, but it
	// is still an int cast to an enum - reject anything outside the two values rather
	// than handing NetworkContext an out-of-range SignalingBackend to switch on.
	if (context == nullptr || (backend != 0 && backend != 1))
	{
		return 0;
	}
	// LAN broadcast ignores the address entirely, so a null pointer there is normal;
	// only the rendezvous backend actually reads it.
	const std::string address = addressUtf8 != nullptr ? std::string{addressUtf8} : std::string{};
	context->ConfigureSignaling(static_cast<aether::net::SignalingBackend>(backend), address);
	return 1;
	});
}

AE_SCRIPT_API std::int32_t aether_net_host_with_code(const char* codeUtf8, std::uint16_t port, std::int32_t maxConnections)
{
	return SafeExport([&] -> std::int32_t
	{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr || codeUtf8 == nullptr)
	{
		return 0;
	}
	return context->HostWithCode(codeUtf8, port, maxConnections) ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_join_by_code(const char* codeUtf8)
{
	return SafeExport([&] -> std::int32_t
	{
	aether::net::NetworkContext* context = Context();
	if (context == nullptr || codeUtf8 == nullptr)
	{
		return 0;
	}
	return context->JoinByCode(codeUtf8) ? 1 : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_traversal_state()
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	// No context is Idle, not a failure - the state before anything has been tried,
	// which is also the only state a single-player build should ever report.
	if (context == nullptr)
	{
		return static_cast<std::int32_t>(aether::net::TraversalState::Idle);
	}
	return static_cast<std::int32_t>(context->GetTraversalState());
	});
}

AE_SCRIPT_API std::int32_t aether_net_traversal_error(char* out, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
	const aether::net::NetworkContext* context = Context();
	return context != nullptr ? CopyOut(context->TraversalFailureReason(), out, capacity) : 0;
	});
}

AE_SCRIPT_API std::int32_t aether_net_new_room_code(char* out, std::int32_t capacity)
{
	return SafeExport([&] -> std::int32_t
	{
	// Independent of any NetworkContext: a room code is just text a hosting player
	// shows on screen before HostWithCode ever runs, so this works with no session.
	return CopyOut(aether::net::NewRoomCode(), out, capacity);
	});
}
