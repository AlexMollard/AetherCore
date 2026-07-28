#include "net/NetworkContext.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "net/NetComponents.hpp"
#include "net/NetRpc.hpp"
#include "net/NetScriptFields.hpp"
#include "net/NetSerialize.hpp"
#include "physics2d/Physics2DSystem.hpp"
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
		// Default player cap for a listen server. Deliberately a local default and
		// not a tunable: a project that needs a different one passes it to StartHost.
		constexpr int kDefaultMaxConnections = 32;

		// Spare ENet peer slots ABOVE the player cap. A joiner has to be accepted at
		// the socket layer before anything can be said to it, so a host sized exactly
		// to its cap would have the socket layer silently refuse the (cap+1)th peer
		// and there would be no link left to send a reason down. The slack is what
		// turns "dropped" into "refused, and here is why".
		constexpr int kRefusalSlack = 4;

		// A body's type is baked into its b2BodyDef at creation and nothing re-reads
		// it afterwards, so changing RigidBody2DComponent::bodyType only takes effect
		// once the backing body is recreated. This is the same route the inspector and
		// MCP take for a body_type edit (RebuildBody2D in Physics2D.reflect.cpp), found
		// the same way - by name, through the world - so nothing about Physics2DSystem
		// has to know networking exists.
		//
		// A world with no physics system registered (a headless test, a 3D scene) is
		// not an error: setting the field is then the whole of the change, and the body
		// will be built with the right type if one is ever created.
		void RebuildBody2D(World& world, Entity entity)
		{
			if (auto* physics = static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem")))
			{
				physics->RebuildBody(world, entity);
			}
		}

		// Puts a handed-over body back on local simulation, at whatever type it was
		// authored as. Both routes back - this client gaining ownership, and the
		// session ending - go through here so the two can never disagree about what
		// "restored" means.
		void ReclaimBody(World& world, Entity entity)
		{
			const auto* marker = world.TryGet<NetSimulationOverride>(entity);
			if (marker == nullptr)
			{
				return;
			}
			const Body2DType authored = marker->authoredBodyType;
			world.Remove<NetSimulationOverride>(entity);
			if (auto* rigid = world.TryGet<RigidBody2DComponent>(entity))
			{
				rigid->bodyType = authored;
				RebuildBody2D(world, entity);
			}
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

	bool NetworkContext::StartHost(World& world, std::uint16_t port, int maxConnections)
	{
		Stop(world);
		m_disconnectReason.clear();
		m_maxConnections = maxConnections > 0 ? maxConnections : kDefaultMaxConnections;
		if (!m_transport.Host(port, m_maxConnections + kRefusalSlack))
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
		// Cleared HERE and nowhere else. Stop() is what runs when a link dies, so a
		// reason cleared there would be gone before the game could read it; clearing it
		// as a new attempt begins is what stops the previous session's reason being
		// mistaken for this one's.
		m_disconnectReason.clear();
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
		// FIRST, while the socket is still up: tell everybody this was deliberate.
		// Without it a client cannot tell a host that quit from a host that crashed or
		// a network that dropped, and those want opposite reactions - accept it and go
		// back to the menu, versus try to come back. Flushed explicitly because the
		// transport is destroyed a few lines below and there is no later service call
		// to push the queue out.
		//
		// Host-only, and only when somebody is listening: a client leaving says nothing
		// (its ENet disconnect is the whole message), and a host with no connections has
		// nobody to tell.
		if (IsHost() && !m_session.Connections().empty())
		{
			m_transport.Broadcast(kChannelReliable, true, EncodeDisconnect(kReasonHostClosed));
			m_transport.Flush();
		}

		// A body this client took off local simulation belongs
		// to the local simulation again the moment there is no session driving it.
		// Scene-placed entities survive Stop, so one left kinematic here is a player
		// character that never falls again in single-player. Stop also runs at the TOP
		// of StartHost/StartClient, which is what un-does a previous client session's
		// handovers when the same process goes on to host.
		RestoreSimulationAuthority(world);

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

	void NetworkContext::RefuseConnection(ConnectionId peer, std::string_view reason)
	{
		if (!IsHost() || peer == kInvalidConnection)
		{
			return;
		}
		// Reliable, then a DEFERRED drop: enet_peer_disconnect_later sends everything
		// already queued for that peer before it tears the link down, which is the only
		// ordering in which the joiner ever sees why. Dropping it immediately would
		// deliver a bare disconnect and the client could not tell a full server from a
		// crashed one.
		m_transport.Send(peer, kChannelReliable, true, EncodeDisconnect(reason));
		m_transport.DisconnectPeer(peer);
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

	void NetworkContext::ApplyRelevancyLeave(World& world, std::uint32_t netId)
	{
		ApplyDespawn(world, netId);
	}

	// Deliberately IsOwner verbatim, not "the host decides everything" - see the note
	// on the declaration. Under client authority the two questions have one answer.
	bool NetworkContext::HasAuthority(World& world, Entity entity) const
	{
		return IsOwner(world, entity);
	}

	bool NetworkContext::IsOwner(World& world, Entity entity) const
	{
		// A stale handle - kept around by a script or a cached reference after the
		// entity behind it died - carries no components at all: entt strips them on
		// destroy(). That made the identity == nullptr branch below misread it as
		// "not replicated, so it's mine", which is the right answer for a genuinely
		// local entity and the wrong one for a dead replicated one. Reject it before
		// the identity lookup even runs, so a destroyed handle can never read as
		// owned - and Entity carries no version (see Entity.hpp), so `valid()` here
		// really is the only test available; it also catches the trivial case of an
		// id that was never created at all.
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return false;
		}
		const auto* identity = world.TryGet<NetworkIdentity>(entity);
		if (identity == nullptr)
		{
			// Not a replicated entity. Offline and on the host that is simply "mine";
			// on a client an unreplicated entity is local-only, so it is mine too.
			return true;
		}
		return OwnsIdentity(*identity);
	}

	bool NetworkContext::OwnsIdentity(const NetworkIdentity& identity) const
	{
		if (m_session.Role() == NetRole::Offline)
		{
			// No session: this process is the whole world, so it owns every entity in
			// it whatever a NetworkIdentity happens to say. The owner id is not
			// cleared when a session ends (only the net id is), so without this an
			// entity left over from a session - or one authored with an owner - would
			// read as somebody else's in single-player and stop being simulated. The
			// documented contract is "offline, both report true"; this is what makes
			// that unconditional rather than true-by-coincidence.
			return true;
		}
		// A client has no connection id of its own until the host's Welcome
		// arrives, and LocalConnection() reads kInvalidConnection until then - the
		// same value every host-owned entity's owner defaults to. Left unguarded,
		// a client briefly "owns" the entire world between Connect() and Welcome.
		// A host's own LocalConnection is kInvalidConnection permanently (that is
		// how it owns its own entities), so the guard only applies to a client.
		if (IsClient() && m_session.LocalConnection() == kInvalidConnection)
		{
			return false;
		}
		return identity.owner == m_session.LocalConnection();
	}

	std::string NetworkContext::ResolveDisplayName(World& world, Entity self, std::string_view desired) const
	{
		std::string wanted = SanitizeReason(desired);
		// Trim, so " " and "" cannot become two different "blank" names.
		const auto notSpace = [](unsigned char c) { return c != ' '; };
		wanted.erase(wanted.begin(), std::find_if(wanted.begin(), wanted.end(), notSpace));
		wanted.erase(std::find_if(wanted.rbegin(), wanted.rend(), notSpace).base(), wanted.end());
		if (wanted.empty())
		{
			wanted = "Player";
		}

		// Where `self` sits in the join order. The host is connection 0, so it is always
		// first and never renamed; the entity id breaks ties between two players one
		// connection happens to own, which keeps the order total.
		const auto* selfIdentity = world.TryGet<NetworkIdentity>(self);
		const ConnectionId selfOwner = selfIdentity != nullptr ? selfIdentity->owner : m_session.LocalConnection();
		const std::pair<ConnectionId, std::uint32_t> selfRank{selfOwner, self.id};

		std::unordered_set<std::string> taken;
		world.View<NetPlayer>().each(
		        [&](entt::entity ent, NetPlayer& player)
		        {
			        const Entity entity = World::FromEntt(ent);
			        if (entity.id == self.id || player.displayName.empty())
			        {
				        return;
			        }
			        const auto* identity = world.TryGet<NetworkIdentity>(entity);
			        const ConnectionId owner = identity != nullptr ? identity->owner : kInvalidConnection;
			        // ONLY players ahead of us in the order. A player behind us will step
			        // aside itself, and treating it as an obstacle here is exactly how two
			        // peers picking the same name at the same moment would both move and
			        // collide again on the next frame.
			        if (std::pair<ConnectionId, std::uint32_t>{owner, entity.id} < selfRank)
			        {
				        taken.insert(player.displayName);
			        }
		        });

		if (!taken.contains(wanted))
		{
			return wanted;
		}
		// " (2)", " (3)", ... Bounded by the number of obstacles plus two, so it always
		// terminates on a free name however contrived the set is.
		for (std::size_t suffix = 2; suffix <= taken.size() + 2; ++suffix)
		{
			std::string candidate = wanted + " (" + std::to_string(suffix) + ")";
			if (!taken.contains(candidate))
			{
				return candidate;
			}
		}
		return wanted;
	}

	std::string NetworkContext::ClaimPlayerName(World& world, Entity self, std::string_view desired)
	{
		// THE OWNER AUTHORS ITS OWN NAME. A display name is replicated state, so the
		// same rule that governs a transform governs it: a peer writing it on somebody
		// else's entity is writing a value that entity's owner will overwrite on its
		// next send, and whether the write survives is a race.
		if (!IsOwner(world, self))
		{
			return {};
		}
		std::string resolved = ResolveDisplayName(world, self, desired);
		if (auto* player = world.TryGet<NetPlayer>(self))
		{
			player->displayName = resolved;
		}
		else
		{
			world.Emplace<NetPlayer>(self, NetPlayer{.displayName = resolved});
		}
		return resolved;
	}

	void NetworkContext::SyncSimulationAuthority(World& world)
	{
		// An offline game has no other authority to defer to, so it never hands a body
		// over. Nothing happens on a client before the Welcome either: until
		// LocalConnection is real, OwnsIdentity answers false for everything (see its
		// guard), and acting on that would hand over this client's OWN character for
		// the length of the handshake and then rebuild its body a second time to give
		// it back. IsConnected() is true for a host the moment it is listening and for
		// a client only once welcomed, which is exactly that rule.
		if (!IsConnected())
		{
			return;
		}

		// Collected first, applied second: both branches below add or remove a
		// component, and RebuildBody2D strips Physics2DStateComponent - all of which
		// invalidate the view being walked.
		std::vector<Entity> handover;
		std::vector<Entity> reclaim;
		world.View<NetworkIdentity, RigidBody2DComponent>().each(
		        [&](entt::entity ent, NetworkIdentity& identity, RigidBody2DComponent& rigid)
		        {
			        if (identity.netId == 0)
			        {
				        return; // not part of this session yet; nothing is replicating it
			        }
			        const Entity entity = World::FromEntt(ent);
			        const bool handedOver = world.Has<NetSimulationOverride>(entity);
			        if (OwnsIdentity(identity))
			        {
				        // Ours to simulate, and the ONLY entity on this machine that is:
				        // scripts drive it, physics integrates it, and the result is what
				        // this peer puts on the wire.
				        if (handedOver)
				        {
					        reclaim.push_back(entity);
				        }
				        return;
			        }
			        // These two together are what make the reconcile idempotent, and
			        // this runs EVERY frame, so an unconditional handover here would
			        // destroy and rebuild the Box2D body of every remote entity on every
			        // tick - and, worse, re-record the CURRENT body type as the authored
			        // one, so the type to restore would decay to Kinematic after a single
			        // frame and nothing could ever be given back.
			        if (handedOver)
			        {
				        return;
			        }
			        // Only a Dynamic body is a problem: Static and Kinematic ones are
			        // already not integrated, and SyncTransforms skips both, so the
			        // replicated transform already survives the frame untouched.
			        if (rigid.bodyType != Body2DType::Dynamic)
			        {
				        return;
			        }
			        handover.push_back(entity);
		        });

		for (const Entity entity: handover)
		{
			auto* rigid = world.TryGet<RigidBody2DComponent>(entity);
			if (rigid == nullptr)
			{
				continue;
			}
			world.EmplaceOrReplace<NetSimulationOverride>(entity,
			        NetSimulationOverride{.authoredBodyType = rigid->bodyType});
			rigid->bodyType = Body2DType::Kinematic;
			RebuildBody2D(world, entity);
		}
		for (const Entity entity: reclaim)
		{
			ReclaimBody(world, entity);
		}
	}

	void NetworkContext::RestoreSimulationAuthority(World& world)
	{
		std::vector<Entity> handedOver;
		world.View<NetSimulationOverride>().each(
		        [&](entt::entity ent, NetSimulationOverride&) { handedOver.push_back(World::FromEntt(ent)); });
		for (const Entity entity: handedOver)
		{
			ReclaimBody(world, entity);
		}
	}
} // namespace aether::net
