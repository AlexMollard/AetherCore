#include "net/NetworkContext.hpp"

#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSerialize.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::net
{
	namespace
	{
		// Default peer budget for a listen server. Deliberately a local default and
		// not a tunable: a project that needs a different one passes it to StartHost.
		constexpr int kDefaultMaxPeers = 32;
	} // namespace

	NetworkContext::NetworkContext(ServiceContainer& services)
	      : m_services(services)
	      , m_schema(BuildReplicationSchema(reflect::ComponentTypes()))
	{
	}

	// Out of line so the header needs only forward declarations of the two bridges.
	NetworkContext::~NetworkContext() = default;

	const std::vector<reflect::ComponentType>& NetworkContext::Catalog() const
	{
		return reflect::ComponentTypes();
	}

	float NetworkContext::Now() const
	{
		const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - m_epoch;
		return elapsed.count();
	}

	// Held for the life of the context rather than rebuilt per frame: the field
	// bridge caches a per-type replicated-property table (invalidated by
	// CSharpScriptingSubsystem's reload generation) that a per-frame temporary would
	// throw away every tick.
	void NetworkContext::SetFieldBridge(std::unique_ptr<ScriptFieldBridge> bridge)
	{
		m_fieldBridge = std::move(bridge);
	}

	void NetworkContext::SetRpcBridge(std::unique_ptr<RpcBridge> bridge)
	{
		m_rpcBridge = std::move(bridge);
	}

	const ScriptFieldBridge* NetworkContext::FieldBridge() const
	{
		return m_fieldBridge.get();
	}

	const RpcBridge* NetworkContext::Rpcs() const
	{
		return m_rpcBridge.get();
	}

	bool NetworkContext::StartHost(World& world, std::uint16_t port, int maxPeers)
	{
		Stop(world);
		if (!m_transport.Host(port, maxPeers > 0 ? maxPeers : kDefaultMaxPeers))
		{
			AE_WARN(LogCategory::App, "Net: host failed - {}", m_transport.LastError());
			return false;
		}
		m_session.SetRole(NetRole::Host);
		m_session.SetLocalConnection(kInvalidConnection);
		// The host's own scene-placed entities get their ids now; every client that
		// loads the same scene derives the identical ids with no handshake.
		ResetForNewSession(world);
		AssignScenePlacedNetIds(world, m_session);
		return true;
	}

	bool NetworkContext::StartClient(World& world, std::string_view host, std::uint16_t port)
	{
		Stop(world);
		if (!m_transport.Connect(host, port))
		{
			AE_WARN(LogCategory::App, "Net: connect failed - {}", m_transport.LastError());
			return false;
		}
		// Role is Client immediately, but LocalConnection stays invalid until the
		// host's Welcome arrives - which is what IsConnected() reports.
		m_session.SetRole(NetRole::Client);
		m_session.SetLocalConnection(kInvalidConnection);
		return true;
	}

	void NetworkContext::Stop(World& world)
	{
		// Everything ApplySpawn/SpawnPrefab instantiated for this session goes with it.
		// Scene-placed entities stay - they came from the scene file and the next
		// session re-derives their ids from it - but a prefab-spawned one has no source
		// but the session that created it. Left behind, ResetForNewSession zeroes its
		// netId, AssignScenePlacedNetIds skips it (no node id), and the next join's
		// replay instantiates a second copy: every join/leave cycle doubles the
		// prefab-spawned population. `scenePlaced` exists precisely to tell them apart.
		//
		// BOTH halves of the predicate are load-bearing. `scenePlaced` is only set by
		// AssignScenePlacedNetIds, so before the first session every scene-placed
		// entity still reads false - and Stop also runs at the TOP of StartHost /
		// StartClient. Testing `scenePlaced` alone would therefore delete the entire
		// replicated scene the moment hosting began. A live net id is what marks an
		// entity as belonging to the session now ending.
		//
		// Collect first, destroy second: DestroyHierarchy mutates the registry the view
		// is iterating.
		std::vector<Entity> spawned;
		world.View<NetworkIdentity>().each(
		        [&](entt::entity ent, NetworkIdentity& identity)
		        {
			        if (identity.netId != 0 && !identity.scenePlaced)
			        {
				        spawned.push_back(World::FromEntt(ent));
			        }
		        });
		for (const Entity entity: spawned)
		{
			ecs::DestroyHierarchy(world, entity);
		}

		m_transport.Disconnect();
		m_session.Clear();
		m_caches.clear();
	}

	void NetworkContext::ForgetNetId(std::uint32_t netId)
	{
		for (auto& [connection, cache]: m_caches)
		{
			cache.Forget(netId);
		}
	}

	std::vector<std::byte> NetworkContext::Frame(NetMessage kind, std::span<const std::byte> payload)
	{
		return FrameMessage(kind, payload);
	}

	std::vector<std::byte> NetworkContext::EncodeWelcome(ConnectionId assigned)
	{
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(NetMessage::Welcome));
		w.U32(assigned);
		return w.Take();
	}

	Entity NetworkContext::SpawnPrefab(World& world, const std::string& prefab, glm::vec3 position, ConnectionId owner)
	{
		if (!IsHost() || prefab.empty())
		{
			return {};
		}
		const auto description = aether::app::scene::ReadPrefabFile(prefab);
		if (!description.has_value())
		{
			AE_WARN(LogCategory::App, "Net: cannot spawn unknown prefab '{}'", prefab);
			return {};
		}

		const glm::mat4 xform = glm::translate(glm::mat4(1.0f), position);
		const Entity root = aether::app::scene::InstantiatePrefab(*description, world,
		        aether::app::scene::MakeApplySceneDeps(m_services), xform);
		if (!root.IsValid())
		{
			return {};
		}

		NetworkIdentity identity{};
		identity.netId = m_session.AllocateNetId();
		identity.owner = owner;
		identity.spawnPrefab = prefab;
		identity.scenePlaced = false;
		world.EmplaceOrReplace<NetworkIdentity>(root, identity);
		m_session.Bind(identity.netId, root);

		const std::vector<std::byte> packet = EncodeSpawn(identity.netId, owner, prefab, position);
		m_transport.Broadcast(kChannelReliable, true, packet);
		return root;
	}

	void NetworkContext::Despawn(World& world, Entity entity)
	{
		if (!entity.IsValid())
		{
			return;
		}
		// A client destroying something it does not own desyncs only itself: the
		// broadcast below is host-only, so the host keeps replicating a net id that now
		// resolves to nothing on this machine and the entity never comes back. Refuse
		// instead. IsOwner is true for an entity with no NetworkIdentity, so a client's
		// purely local entities are still destroyable through Net.Despawn.
		if (IsClient() && !IsOwner(world, entity))
		{
			return;
		}
		const std::uint32_t netId = m_session.NetIdFor(entity);
		if (IsHost() && netId != 0)
		{
			const std::vector<std::byte> packet = EncodeDespawn(netId);
			m_transport.Broadcast(kChannelReliable, true, packet);
		}
		if (netId != 0)
		{
			m_session.Unbind(netId);
			ForgetNetId(netId);
		}
		// A prefab instantiates as a hierarchy; destroying only the root would strand
		// every child in the world with no owner and no way to reach them.
		ecs::DestroyHierarchy(world, entity);
	}

	void NetworkContext::ApplySpawn(World& world, const SpawnMessage& msg)
	{
		if (m_session.EntityFor(msg.netId).IsValid())
		{
			// Already bound. This is the common case for the joining-client replay of
			// a scene-placed entity: both ends derived the same id from the scene file,
			// so there is nothing to create.
			return;
		}
		if (!IsSafePrefabName(msg.prefab))
		{
			// Either a scene-placed entity this client does not have - nothing generic
			// can recreate it, since the framework does not know how the project loads
			// scenes - or a name a peer built to reach outside the prefab directory.
			return;
		}
		const auto description = aether::app::scene::ReadPrefabFile(msg.prefab);
		if (!description.has_value())
		{
			AE_WARN(LogCategory::App, "Net: spawn names prefab '{}', which this build has no asset for", msg.prefab);
			return;
		}

		const glm::mat4 xform = glm::translate(glm::mat4(1.0f), msg.position);
		const Entity root = aether::app::scene::InstantiatePrefab(*description, world,
		        aether::app::scene::MakeApplySceneDeps(m_services), xform);
		if (!root.IsValid())
		{
			return;
		}

		NetworkIdentity identity{};
		identity.netId = msg.netId;
		identity.owner = msg.owner;
		identity.spawnPrefab = msg.prefab;
		identity.scenePlaced = false;
		world.EmplaceOrReplace<NetworkIdentity>(root, identity);
		m_session.Bind(identity.netId, root);
	}

	void NetworkContext::ResetForNewSession(World& world)
	{
		world.View<NetworkIdentity>().each([](NetworkIdentity& identity) { identity.netId = 0; });
	}

	void NetworkContext::ApplyDespawn(World& world, std::uint32_t netId)
	{
		const Entity entity = m_session.EntityFor(netId);
		m_session.Unbind(netId);
		ForgetNetId(netId);
		if (entity.IsValid())
		{
			ecs::DestroyHierarchy(world, entity);
		}
	}

	bool NetworkContext::HasAuthority(World& world, Entity entity) const
	{
		if (!IsClient())
		{
			return true; // host, or offline: local state is the only state
		}
		return IsOwner(world, entity);
	}

	bool NetworkContext::IsOwner(World& world, Entity entity) const
	{
		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity == nullptr)
		{
			// Not a replicated entity. Offline and on the host that is simply "mine";
			// on a client an unreplicated entity is local-only, so it is mine too.
			return true;
		}
		// A client has no connection id of its own until the host's Welcome
		// arrives, and LocalConnection() reads kInvalidConnection until then - the
		// same value every host-owned entity's owner defaults to. Left unguarded,
		// a client briefly "owns" the entire world between Connect() and Welcome.
		// A host's own LocalConnection is kInvalidConnection permanently (that is
		// how it owns its own entities), so the guard only applies to a client.
		// See the identical guard on the transform-resolution path in
		// NetworkSystems.cpp.
		if (IsClient() && m_session.LocalConnection() == kInvalidConnection)
		{
			return false;
		}
		return identity->owner == m_session.LocalConnection();
	}
} // namespace aether::net
