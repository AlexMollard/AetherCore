#include "net/NetworkContext.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "net/CSharpRpcBridge.hpp"
#include "net/CSharpScriptFieldBridge.hpp"
#include "net/NetComponents.hpp"
#include "net/NetSerialize.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::net
{
	namespace
	{
		// Default peer budget for a listen server. Deliberately a local default and
		// not a tunable: a project that needs a different one passes it to StartHost.
		constexpr int kDefaultMaxPeers = 32;

		// A prefab name arrives from a remote peer and is used to open a file. Reject
		// anything that could escape the prefab directory before it reaches the loader.
		bool IsSafePrefabName(std::string_view name)
		{
			return !name.empty() && name.find("..") == std::string_view::npos
			       && name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos
			       && name.find(':') == std::string_view::npos;
		}

		// Clears every net id in the world. A session assigns scene-placed ids by
		// walking entities whose id is still 0, so ids left behind by a previous
		// session would make the next one silently skip those entities and never bind
		// them - they would exist, be replicated by nobody, and update never.
		void ResetNetIds(World& world)
		{
			world.View<NetworkIdentity>().each([](NetworkIdentity& identity) { identity.netId = 0; });
		}
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

	const ScriptFieldBridge* NetworkContext::FieldBridge() const
	{
		if (m_fieldBridge != nullptr)
		{
			return m_fieldBridge.get();
		}
		const auto* scripting = m_services.TryGet<aether::app::scripting::CSharpScriptingSubsystem>();
		const auto* instances = m_services.TryGet<aether::app::ScriptComponentSystem>();
		if (scripting == nullptr || instances == nullptr)
		{
			return nullptr; // no CLR, or the script system is not registered yet
		}
		// Built once and kept: the bridge caches a per-type replicated-property table
		// (invalidated by CSharpScriptingSubsystem's reload generation), which a
		// per-frame temporary would throw away every tick.
		m_fieldBridge = std::make_unique<CSharpScriptFieldBridge>(*scripting, *instances);
		return m_fieldBridge.get();
	}

	const RpcBridge* NetworkContext::Rpcs() const
	{
		if (m_rpcBridge != nullptr)
		{
			return m_rpcBridge.get();
		}
		const auto* scripting = m_services.TryGet<aether::app::scripting::CSharpScriptingSubsystem>();
		const auto* instances = m_services.TryGet<aether::app::ScriptComponentSystem>();
		if (scripting == nullptr || instances == nullptr)
		{
			return nullptr;
		}
		m_rpcBridge = std::make_unique<CSharpRpcBridge>(*scripting, *instances);
		return m_rpcBridge.get();
	}

	bool NetworkContext::StartHost(World& world, std::uint16_t port, int maxPeers)
	{
		Stop();
		if (!m_transport.Host(port, maxPeers > 0 ? maxPeers : kDefaultMaxPeers))
		{
			AE_WARN(LogCategory::App, "Net: host failed - {}", m_transport.LastError());
			return false;
		}
		m_session.SetRole(NetRole::Host);
		m_session.SetLocalConnection(kInvalidConnection);
		// The host's own scene-placed entities get their ids now; every client that
		// loads the same scene derives the identical ids with no handshake.
		ResetNetIds(world);
		AssignScenePlacedNetIds(world, m_session);
		return true;
	}

	bool NetworkContext::StartClient(std::string_view host, std::uint16_t port)
	{
		Stop();
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

	void NetworkContext::Stop()
	{
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
		ByteWriter w;
		w.U8(static_cast<std::uint8_t>(kind));
		w.Bytes(payload);
		return w.Take();
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
		ResetNetIds(world);
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
		return identity->owner == m_session.LocalConnection();
	}
} // namespace aether::net
