#include "systems/ScriptComponentSystem.hpp"

#include <utility>
#include <vector>

#include "IEngineRuntime.hpp"
#include "net/NetworkContext.hpp"
#include "net/NetworkSystems.hpp"
#include "scene/Components.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSerializerDetail.hpp"
#include "scene/SceneStreamingDependencies.hpp"
#include "scene/SceneStreamingReferences.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayHost.hpp"
#include "scripting/SceneContext.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiNavigationSystem.hpp"
#include "ui/UiTextBoxSystem.hpp"
#include "ui/UiWidgetSystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app
{
	namespace
	{
		std::uint64_t InstanceKey(std::uint32_t entityId, std::uint32_t scriptIndex)
		{
			return (static_cast<std::uint64_t>(entityId) << 32) | static_cast<std::uint64_t>(scriptIndex);
		}

		std::uint32_t InstanceEntityId(std::uint64_t key)
		{
			return static_cast<std::uint32_t>(key >> 32);
		}

		std::uint32_t InstanceScriptIndex(std::uint64_t key)
		{
			return static_cast<std::uint32_t>(key & 0xffffffffu);
		}

		// ActiveContextScope now lives in SceneContext.hpp: the RPC bridge needs the
		// same guard around its own managed dispatch, and two definitions of "publish
		// the active context" would be two places to get the restore rule wrong.
		using scripting::ActiveContextScope;

		// Re-resolve a script entry by (entity, index). Returns null when the entity
		// died or the entry vanished since the last fetch - both possible after any
		// managed callback (deferred destroy, script list edit, pool realloc).
		ScriptEntry* FindScriptEntry(World& world, Entity entity, std::uint32_t scriptIndex)
		{
			if (!world.GetRegistry().valid(World::ToEntt(entity)))
			{
				return nullptr;
			}
			auto* sc = world.TryGet<ScriptComponent>(entity);
			if (sc == nullptr || scriptIndex >= sc->scripts.size())
			{
				return nullptr;
			}
			return &sc->scripts[scriptIndex];
		}
	} // namespace

	ScriptComponentSystem::~ScriptComponentSystem()
	{
		// Explicit Invalidate runs while the scene context and provider are alive.
		m_instances.clear();
	}

	std::uint64_t ScriptComponentSystem::GetInstanceHandle(std::uint32_t entityId, std::uint32_t scriptIndex) const
	{
		const auto it = m_instances.find(InstanceKey(entityId, scriptIndex));
		return it != m_instances.end() ? it->second.handle : 0;
	}

	// See the header for the full contract (what this closes, what it cannot).
	void ScriptComponentSystem::SyncPropertiesFromLiveInstance(Entity entity, std::uint32_t scriptIndex, ScriptEntry& entry) const
	{
		const std::uint64_t handle = GetInstanceHandle(entity.id, scriptIndex);
		if (handle == 0)
		{
			if (entry.attached)
			{
				// Genuinely inconsistent - attached claims a live instance exists.
				// Not the ordinary "never attached this session" case below, which
				// is silent because the cache already holds the only value that
				// ever existed.
				AE_WARN(LogCategory::App, "ScriptComponent: '{}' on entity {} is marked attached but has no live instance - saving its last-known property cache instead of current values", entry.path, entity.id);
			}
			return;
		}
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		if (host == nullptr)
		{
			return;
		}
		const auto props = host->GetScriptProperties(entry.path);
		for (std::size_t i = 0; i < props.size(); ++i)
		{
			ScriptPropertyValue value;
			value.type = props[i].type;
			if (host->GetPropertyValue(handle, static_cast<int>(i), value))
			{
				entry.properties[props[i].name] = value;
			}
		}
	}

	// See the header for the known/settled state each Instance tracks. The actual
	// "what does this entity's ownership read as right now" question is
	// net::QueryOwnership's - kept there (and unit-tested there, in
	// tests/net/NetworkSystemsTests.cpp) rather than reimplemented here, so this
	// function is only ever the state-machine/dispatch half: did that answer just
	// become decidable, or change, and if so, call the appended
	// gameplay host's ownership event.
	//
	// `instance.settled` retires an unreplicated entity's instance after its first
	// (necessarily unconditional, per QueryOwnership) firing: it has no `owner`
	// field to ever change, so there is nothing left to check again. That is every
	// non-networked script in a project - the per-frame cost this hook adds for
	// them is one bool check, forever, the same "a hook nobody uses costs nothing
	// ongoing" property DispatchPhysicsEvents gives EntityScript.
	bool ScriptComponentSystem::DispatchOwnershipChanged(World& world, Entity entity, scripting::GameplayHost& host, std::uint64_t handle, Instance& instance)
	{
		if (instance.settled)
		{
			return true;
		}

		auto* context = m_services.TryGet<net::NetworkContext>();
		const net::OwnershipQuery query = net::QueryOwnership(world, context, entity);
		if (!query.known)
		{
			return true;
		}

		const bool firstFire = !instance.known;
		if (firstFire || instance.isOwner != query.isOwner || instance.owner != query.owner)
		{
			instance.known = true;
			instance.isOwner = query.isOwner;
			instance.owner = query.owner;
			if ((host.Capabilities() & AE_GAMEPLAY_CAP_OWNERSHIP) != 0)
			{
				if (!host.DispatchOwnershipChanged(handle, query.owner, query.isOwner))
				{
					return false;
				}
			}
		}

		if (!query.replicated)
		{
			instance.settled = true; // no `owner` field left that could ever change this again
		}
		return true;
	}

	bool ScriptComponentSystem::UpdateScriptEntity(scripting::GameplayHost& host, scripting::SceneContext& ctx, Entity entity, std::uint32_t scriptIndex, ScriptEntry& script, float dt, bool attachOnly)
	{
		const scripting::ActiveScriptEntityScope entityScope(ctx, entity);
		if (!host.IsAvailable())
		{
			return false;
		}

		// The host calls below (including constructors and user callbacks) can add a ScriptComponent to any entity via
		// aether_add_script, growing the entt pool and dangling every
		// ScriptComponent*/ScriptEntry& held across the call. So typeName is a copy,
		// and the entry is re-fetched from the world after every managed call.
		ScriptEntry* entry = &script;
		const std::string typeName = entry->path;
		if (m_failedTypes.contains(typeName))
		{
			return false;
		}

		const std::uint64_t key = InstanceKey(entity.id, scriptIndex);
		if (m_failedInstances.contains(key))
		{
			return false;
		}
		const bool native = host.Language() == "rust";
		auto fail = [&](std::uint64_t failedHandle)
		{
			(void) host.Detach(failedHandle);
			(void) host.Destroy(failedHandle);
			m_instances.erase(key);
			m_failedInstances.insert(key);
			if (auto* current = FindScriptEntry(*ctx.world, entity, scriptIndex))
			{
				current->attached = false;
			}
			return false;
		};
		std::uint64_t handle = 0;
		if (const auto it = m_instances.find(key); it != m_instances.end())
		{
			handle = it->second.handle;
		}

		if (!entry->attached && handle != 0)
		{
			(void) host.Detach(handle);
			(void) host.Destroy(handle);
			m_instances.erase(key);
			handle = 0;
			entry = FindScriptEntry(*ctx.world, entity, scriptIndex);
			if (entry == nullptr)
			{
				return false;
			}
		}

		if (handle == 0)
		{
			if ((host.Capabilities() & AE_GAMEPLAY_CAP_PHYSICS) != 0 && !ctx.world->Has<CollisionEvents2DComponent>(entity))
			{
				ctx.world->Emplace<CollisionEvents2DComponent>(entity);
			}
			if (native && ctx.physics != nullptr && (host.Capabilities() & AE_GAMEPLAY_CAP_PHYSICS) != 0 && !ctx.world->Has<CollisionEventsComponent>(entity))
			{
				// 3D contact/trigger lists are only filled for entities that carry this component.
				ctx.world->Emplace<CollisionEventsComponent>(entity);
			}
			const auto created = host.CreateInstance(typeName, entity.id);
			if (!created || *created == 0)
			{
				AE_WARN(LogCategory::App, "ScriptComponent: type '{}' failed to instantiate - fields retained, disabled until reload", typeName);
				if (native)
				{
					m_failedInstances.insert(key);
				}
				else
				{
					m_failedTypes.insert(typeName);
				}
				return false;
			}
			handle = *created;
			m_instances[key] = Instance{.handle = handle, .typeName = typeName};
			// The constructor may have grown the pool or killed the entry.
			entry = FindScriptEntry(*ctx.world, entity, scriptIndex);
			if (entry == nullptr)
			{
				// Orphaned instance; PurgeStaleInstances reclaims it below.
				return false;
			}
			const auto properties = entry->properties;
			const auto applied = host.TryApplyProperties(handle, typeName, properties);
			if (native && !applied)
			{
				return fail(handle);
			}
			entry = FindScriptEntry(*ctx.world, entity, scriptIndex);
			if (entry == nullptr)
			{
				return false;
			}
			entry->attached = false; // a freshly created instance must attach
		}

		if (!entry->attached)
		{
			entry->attached = true;
			const auto attached = host.Attach(handle);
			if (native && !attached)
			{
				return fail(handle);
			}
		}

		if (attachOnly)
		{
			return true;
		}

		// Every frame, not just on attach: this is also how a later Welcome landing
		// or an ownership hand-off (see NetworkSystems.cpp's PruneDisconnected) gets
		// noticed on an instance that has been live for a while.
		if (!DispatchOwnershipChanged(*ctx.world, entity, host, handle, m_instances[key]) && native)
		{
			return fail(handle);
		}

		if ((host.Capabilities() & AE_GAMEPLAY_CAP_NETWORK) != 0)
		{
			if (const auto* receive = static_cast<const aether::net::NetworkReceiveSystem*>(ctx.world->FindSystem("NetworkReceiveSystem")))
			{
				for (const auto& event : receive->GameplayEvents())
				{
					if (!host.DispatchNetwork(handle, event.code, event.peer) && native)
					{
						return fail(handle);
					}
				}
			}
		}

		// Snapshot the event lists before calling user code: callbacks may add components
		// and invalidate ECS storage. Managed scripts retain their own event polling.
		if ((host.Capabilities() & AE_GAMEPLAY_CAP_PHYSICS) != 0)
		{
			const auto dispatch = [&](const auto& snapshot, std::uint32_t flags)
			{
				const std::vector<Entity>* lists[] = {&snapshot.collisionEnter, &snapshot.collisionExit, &snapshot.triggerEnter, &snapshot.triggerExit, &snapshot.overlapping};
				for (std::uint32_t kind = 0; kind < 5; ++kind)
				{
					for (const auto other: *lists[kind])
					{
						if (!host.DispatchPhysics(handle, other.id, kind, flags) && native)
						{
							return false;
						}
					}
				}
				return true;
			};
			if (const auto* events = ctx.world->TryGet<CollisionEvents2DComponent>(entity))
			{
				if (!dispatch(CollisionEvents2DComponent(*events), 0))
				{
					return fail(handle);
				}
			}
			if (const auto* events = ctx.world->TryGet<CollisionEventsComponent>(entity))
			{
				if (!dispatch(CollisionEventsComponent(*events), AE_GAMEPLAY_PHYSICS_FLAG_3D))
				{
					return fail(handle);
				}
			}
		}
		const auto updated = host.Update(handle, dt);
		if (native && !updated)
		{
			return fail(handle);
		}
		return true;
	}

	void ScriptComponentSystem::PurgeStaleInstances(World& world, scripting::GameplayHost& host, scripting::SceneContext& ctx)
	{
		if (!host.IsAvailable() || m_instances.empty())
		{
			return;
		}

		auto& reg = world.GetRegistry();
		std::vector<std::uint64_t> stale;
		for (const auto& [key, instance]: m_instances)
		{
			const std::uint32_t id = InstanceEntityId(key);
			const std::uint32_t scriptIndex = InstanceScriptIndex(key);
			const auto enttE = World::ToEntt(Entity{id});
			const bool alive = reg.valid(enttE);
			const auto* sc = alive ? world.TryGet<ScriptComponent>(Entity{id}) : nullptr;
			const bool stillScripted = sc != nullptr && scriptIndex < sc->scripts.size() && !sc->scripts[scriptIndex].path.empty() && sc->scripts[scriptIndex].path == instance.typeName;
			if (!stillScripted)
			{
				stale.push_back(key);
			}
		}

		if (stale.empty())
		{
			return;
		}

		const ActiveContextScope scope(ctx);
		for (const std::uint64_t key: stale)
		{
			const scripting::ActiveScriptEntityScope entityScope(ctx, Entity{InstanceEntityId(key)});
			const std::uint64_t handle = m_instances[key].handle;
			(void) host.Detach(handle);
			(void) host.Destroy(handle);
			m_instances.erase(key);
		}
	}

	void ScriptComponentSystem::DestroyAllInstances(scripting::GameplayHost& host, scripting::SceneContext& ctx)
	{
		if (!host.IsAvailable() || m_instances.empty())
		{
			m_instances.clear();
			return;
		}

		const ActiveContextScope scope(ctx);
		for (const auto& [key, instance]: m_instances)
		{
			const scripting::ActiveScriptEntityScope entityScope(ctx, Entity{InstanceEntityId(key)});
			(void) host.Detach(instance.handle);
			(void) host.Destroy(instance.handle);
		}
		m_instances.clear();
	}

	Expected<std::optional<ScriptComponentSystem::StreamingScriptState>> ScriptComponentSystem::CaptureStreamingScript(World& world, Entity entity, std::uint32_t scriptIndex)
	{
		auto* scripts = world.TryGet<ScriptComponent>(entity);
		if (scripts == nullptr || scriptIndex >= scripts->scripts.size())
			return std::unexpected(AetherError::Engine("Streaming script entry is missing"));
		auto* ctx = m_services.TryGet<scripting::SceneContext>();
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		if (ctx == nullptr || host == nullptr || !host->IsAvailable())
			return std::unexpected(AetherError::Engine("Cannot capture streaming scripts without a gameplay host"));
		auto& script = scripts->scripts[scriptIndex];
		const auto handle = GetInstanceHandle(entity.id, scriptIndex);
		if (handle == 0)
		{
			if (script.attached)
				return std::unexpected(AetherError::Engine("Attached streaming script instance is missing"));
			return std::optional<StreamingScriptState>{};
		}
		const ActiveContextScope scope(*ctx);
		const scripting::ActiveScriptEntityScope entityScope(*ctx, entity);
		const auto properties = host->GetScriptProperties(script.path);
		for (std::size_t property = 0; property < properties.size(); ++property)
		{
			ScriptPropertyValue value;
			value.type = properties[property].type;
			if (!host->GetPropertyValue(handle, static_cast<int>(property), value))
				return std::unexpected(AetherError::Engine("Failed to capture streaming script property"));
			script.properties[properties[property].name] = value;
		}
		const auto* node = world.TryGet<SceneNodeComponent>(entity);
		if (node == nullptr || node->id == 0)
			return std::unexpected(AetherError::Engine("Streaming script has no stable scene node"));
		const auto nodeId = node->id;
		auto state = host->CaptureStreamingState(handle);
		if (!state) return std::unexpected(std::move(state.error()));
		return std::optional<StreamingScriptState>{StreamingScriptState{nodeId, scriptIndex, std::move(*state)}};
	}

	Expected<std::vector<ScriptComponentSystem::StreamingScriptState>> ScriptComponentSystem::CaptureStreamingScripts(World& world, const std::vector<Entity>& entities)
	{
		std::vector<StreamingScriptState> result;
		for (const auto entity : entities)
		{
			const auto* scripts = world.TryGet<ScriptComponent>(entity);
			const std::size_t count = scripts != nullptr ? scripts->scripts.size() : 0;
			for (std::size_t i = 0; i < count; ++i)
			{
				auto state = CaptureStreamingScript(world, entity, static_cast<std::uint32_t>(i));
				if (!state) return std::unexpected(std::move(state.error()));
				if (*state) result.push_back(std::move(**state));
			}
		}
		return result;
	}

	Expected<void> ScriptComponentSystem::RestoreStreamingScripts(World& world, const std::vector<Entity>& entities, const std::vector<StreamingScriptState>& states)
	{
		if (states.empty()) return {};
		auto* ctx = m_services.TryGet<scripting::SceneContext>();
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		if (ctx == nullptr || host == nullptr || !host->IsAvailable())
			return std::unexpected(AetherError::Engine("Cannot restore streaming scripts without a gameplay host"));
		AttachPredicted(world, entities);
		std::unordered_map<std::uint64_t, Entity> nodes;
		for (const auto entity : entities)
			if (const auto* node = world.TryGet<SceneNodeComponent>(entity); node != nullptr && node->id != 0)
				if (!nodes.emplace(node->id, entity).second)
					return std::unexpected(AetherError::Engine("Duplicate streaming scene node"));
		const ActiveContextScope scope(*ctx);
		for (const auto& state : states)
		{
			const auto node = nodes.find(state.node);
			if (node == nodes.end()) return std::unexpected(AetherError::Engine("Streaming script node missing"));
			const auto handle = GetInstanceHandle(node->second.id, state.scriptIndex);
			const scripting::ActiveScriptEntityScope entityScope(*ctx, node->second);
			if (handle == 0) return std::unexpected(AetherError::Engine("Streaming script instance missing"));
			auto restored = host->RestoreStreamingState(handle, state.state);
			if (!restored) return std::unexpected(std::move(restored.error()));
		}
		return {};
	}

	Expected<void> ScriptComponentSystem::PrepareStreamingScriptReferences(World& world, const std::vector<Entity>& entities)
	{
		auto* ctx = m_services.TryGet<scripting::SceneContext>();
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		std::unordered_set<std::uint32_t> detached;
		for (const auto entity : entities) detached.insert(entity.id);
		auto& registry = world.GetRegistry();
		auto& pending = registry.ctx().contains<scene::detail::StreamingScriptNodeReferences>()
		    ? registry.ctx().get<scene::detail::StreamingScriptNodeReferences>()
		    : registry.ctx().emplace<scene::detail::StreamingScriptNodeReferences>();
		struct Change { Entity owner; std::uint32_t index; std::string property; std::uint64_t ownerNode; std::uint64_t targetNode; };
		std::vector<Change> changes;
		bool invalid = false;
		world.View<ScriptComponent>().each([&](entt::entity raw, ScriptComponent& component)
		{
			const Entity owner = World::FromEntt(raw);
			if (detached.contains(owner.id)) return;
			for (std::size_t index = 0; index < component.scripts.size(); ++index)
			{
				auto& script = component.scripts[index];
				const auto handle = GetInstanceHandle(owner.id, static_cast<std::uint32_t>(index));
				if (handle != 0)
				{
					if (!host || !ctx || !host->IsAvailable()) { invalid = true; return; }
					const ActiveContextScope scope(*ctx);
					const auto properties = host->GetScriptProperties(script.path);
					for (std::size_t p = 0; p < properties.size(); ++p)
					{
						if (properties[p].type != ScriptPropertyValue::Type::Entity && properties[p].type != ScriptPropertyValue::Type::Component) continue;
						ScriptPropertyValue value;
						value.type = properties[p].type;
						if (!host->GetPropertyValue(handle, static_cast<int>(p), value)) { invalid = true; return; }
						script.properties[properties[p].name] = value;
					}
				}
				for (const auto& [name, value] : script.properties)
				{
					if ((value.type != ScriptPropertyValue::Type::Entity && value.type != ScriptPropertyValue::Type::Component) ||
					    value.i64 <= 0 || !detached.contains(static_cast<std::uint32_t>(value.i64))) continue;
					const auto* ownerNode = world.TryGet<SceneNodeComponent>(owner);
					const auto* targetNode = world.TryGet<SceneNodeComponent>(Entity{static_cast<std::uint32_t>(value.i64)});
					if (!ownerNode || !ownerNode->id || !targetNode || !targetNode->id) { invalid = true; return; }
					changes.push_back({owner, static_cast<std::uint32_t>(index), name, ownerNode->id, targetNode->id});
				}
			}
		});
		if (invalid) return std::unexpected(AetherError::Engine("Cannot preserve streaming script reference without a live host and stable nodes"));
		for (const auto& change : changes)
		{
			auto* component = world.TryGet<ScriptComponent>(change.owner);
			if (!component || change.index >= component->scripts.size()) return std::unexpected(AetherError::Engine("Streaming script owner changed during detach"));
			auto& script = component->scripts[change.index];
			auto found = script.properties.find(change.property);
			if (found == script.properties.end()) return std::unexpected(AetherError::Engine("Streaming script property disappeared"));
			auto value = found->second;
			value.i64 = 0;
			const auto handle = GetInstanceHandle(change.owner.id, change.index);
			if (handle != 0)
			{
				const int index = host->FindPropertyIndex(script.path, change.property);
				if (index < 0) return std::unexpected(AetherError::Engine("Streaming script property is unavailable"));
				const ActiveContextScope scope(*ctx);
				auto result = host->TrySetPropertyValue(handle, index, value);
				if (!result) return std::unexpected(std::move(result.error()));
			}
			found->second = value;
			std::erase_if(pending.entries, [&](const auto& ref) { return ref.ownerNode == change.ownerNode && ref.scriptIndex == change.index && ref.property == change.property; });
			pending.entries.push_back({change.ownerNode, change.targetNode, change.index, script.path, change.property, 0});
		}
		return {};
	}

	Expected<void> ScriptComponentSystem::ResolveStreamingScriptReferences(World& world)
	{
		auto* pending = world.GetRegistry().ctx().find<scene::detail::StreamingScriptNodeReferences>();
		if (!pending || pending->entries.empty()) return {};
		auto* ctx = m_services.TryGet<scripting::SceneContext>();
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		const scene::StreamingNodeIndex nodes(world);
		if (!nodes.ambiguousExact.empty()) return std::unexpected(AetherError::Engine("Duplicate scene node while resolving streaming script references"));
		for (auto it = pending->entries.begin(); it != pending->entries.end();)
		{
			const auto owner = nodes.exact.find(it->ownerNode);
			if (owner == nodes.exact.end()) { ++it; continue; }
			auto* component = world.TryGet<ScriptComponent>(owner->second);
			if (!component || it->scriptIndex >= component->scripts.size() || component->scripts[it->scriptIndex].path != it->scriptType)
			{
				it = pending->entries.erase(it); continue;
			}
			auto& script = component->scripts[it->scriptIndex];
			const auto property = script.properties.find(it->property);
			if (property == script.properties.end()) { it = pending->entries.erase(it); continue; }
			const auto handle = GetInstanceHandle(owner->second.id, static_cast<std::uint32_t>(it->scriptIndex));
			const int index = handle && host ? host->FindPropertyIndex(script.path, it->property) : -1;
			if (handle)
			{
				if (!ctx || !host || index < 0) return std::unexpected(AetherError::Engine("Cannot inspect live streaming script reference"));
				ScriptPropertyValue current = property->second;
				const ActiveContextScope scope(*ctx);
				if (!host->GetPropertyValue(handle, index, current)) return std::unexpected(AetherError::Engine("Cannot read live streaming script reference"));
				property->second = current;
			}
			if (property->second.i64 != it->expected) { it = pending->entries.erase(it); continue; }
			const auto target = nodes.Resolve(it->targetNode);
			if (!target) return std::unexpected(target.error());
			if (!target->IsValid()) { ++it; continue; }
			auto value = property->second;
			value.i64 = target->id;
			if (handle)
			{
				const ActiveContextScope scope(*ctx);
				auto result = host->TrySetPropertyValue(handle, index, value);
				if (!result) return std::unexpected(std::move(result.error()));
			}
			property->second = value;
			it = pending->entries.erase(it);
		}
		return {};
	}

	Expected<void> ScriptComponentSystem::DetachStreamingEntities(const std::vector<Entity>& entities)
	{
		auto* ctx = m_services.TryGet<scripting::SceneContext>();
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		std::unordered_set<std::uint32_t> ids;
		for (const auto entity : entities) ids.insert(entity.id);
		std::vector<std::uint64_t> keys;
		for (const auto& [key, instance] : m_instances)
			if (ids.contains(InstanceEntityId(key))) keys.push_back(key);
		if (keys.empty()) return {};
		if (ctx == nullptr || ctx->world == nullptr || host == nullptr || !host->IsAvailable())
			return std::unexpected(AetherError::Engine("Cannot detach streaming scripts without a gameplay host"));
		const ActiveContextScope scope(*ctx);
		for (const auto key : keys)
		{
			const auto it = m_instances.find(key);
			if (it == m_instances.end()) continue;
			const auto handle = it->second.handle;
			const Entity entity{InstanceEntityId(key)};
			const scripting::ActiveScriptEntityScope entityScope(*ctx, entity);
			auto detached = host->Detach(handle);
			if (!detached) return std::unexpected(std::move(detached.error()));
			auto destroyed = host->Destroy(handle);
			if (!destroyed)
			{
				auto reattached = host->Attach(handle);
				if (!reattached)
					return std::unexpected(AetherError::Engine("Streaming script destroy failed and reattach failed: " + destroyed.error().message + "; " + reattached.error().message));
				return std::unexpected(std::move(destroyed.error()));
			}
			const auto current = m_instances.find(key);
			if (current != m_instances.end() && current->second.handle == handle) m_instances.erase(current);
			if (auto* entry = FindScriptEntry(*ctx->world, entity, InstanceScriptIndex(key))) entry->attached = false;
		}
		return {};
	}

	void ScriptComponentSystem::AttachPredicted(World& world, const std::vector<Entity>& entities)
	{
		auto* sceneCtx = m_services.TryGet<scripting::SceneContext>();
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		if (sceneCtx == nullptr || host == nullptr || !host->IsAvailable())
			return;
		for (const Entity entity: entities)
		{
			for (std::size_t i = 0; world.GetRegistry().valid(World::ToEntt(entity)); ++i)
			{
				auto* scripts = world.TryGet<ScriptComponent>(entity);
				if (scripts == nullptr || i >= scripts->scripts.size())
					break;
				if (!scripts->scripts[i].path.empty())
				{
					const ActiveContextScope scope(*sceneCtx);
					UpdateScriptEntity(*host, *sceneCtx, entity, static_cast<std::uint32_t>(i), scripts->scripts[i], 0.0f, true);
				}
			}
		}
	}

	void ScriptComponentSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		auto* sceneCtx = m_services.TryGet<scripting::SceneContext>();
		if (sceneCtx == nullptr)
		{
			return;
		}
		auto* host = m_services.TryGet<scripting::GameplayHost>();
		if (host == nullptr || !host->IsAvailable())
		{
			return;
		}

		sceneCtx->deltaTime = dt;
		sceneCtx->elapsedTime += dt;
		++sceneCtx->frameCount;

		// Drive UI focus/activation before scripts read it (Ui.IsFocused / Ui.WasActivated),
		// then widgets consume that focus/activation (slider adjust, toggle flip) so scripts
		// see this frame's committed values + change flags.
		if (sceneCtx->input != nullptr)
		{
			// Text editing runs on the UNSCALED clock - the same wall-clock counter Time.Unscaled
			// reads. `elapsedTime` accumulates scaled dt, so at timeScale = 0 the text box derives
			// dt = 0: held Backspace would delete exactly one character and the caret would freeze
			// mid-blink. A text field on a pause menu or settings overlay is exactly where this
			// lands. Falls back to the scaled clock when there is no runtime (headless tools).
			const float uiTime = sceneCtx->engineRuntime != nullptr ? static_cast<float>(sceneCtx->engineRuntime->RealElapsedSeconds()) : sceneCtx->elapsedTime;
			aether::ui::UiNavigationSystem::Update(world, *sceneCtx->input);
			aether::ui::UiWidgetSystem::Update(world, *sceneCtx->input, static_cast<float>(sceneCtx->elapsedTime));
			aether::ui::UiTextBoxSystem::Update(world, *sceneCtx->input, m_services.TryGet<aether::ui::FontRegistry>(), uiTime);
		}

		std::vector<Entity> scripted;
		for (const auto e: world.GetRegistry().view<ScriptComponent>())
		{
			scripted.push_back(World::FromEntt(e));
		}

		auto& reg = world.GetRegistry();
		for (const Entity e: scripted)
		{
			if (!reg.valid(World::ToEntt(e)))
			{
				continue;
			}
			if (ecs::HasDisabledAncestor(world, e))
			{
				continue;
			}
			// Re-fetch the component EVERY iteration; nothing derived from it crosses
			// UpdateScriptEntity. Its callbacks can call World.AddScript on an
			// entity without a ScriptComponent yet, and that Emplace grows/reallocates
			// the entt pool - dangling any ScriptComponent* or ScriptEntry& held here.
			// The per-iteration validity/index re-check also ends the loop cleanly when a
			// callback strips the component or (despite the deferred destroy) kills the
			// entity mid-loop.
			for (std::size_t i = 0;; ++i)
			{
				if (!reg.valid(World::ToEntt(e)))
				{
					break;
				}
				auto* sc = world.TryGet<ScriptComponent>(e);
				if (sc == nullptr || i >= sc->scripts.size())
				{
					break;
				}
				ScriptEntry& script = sc->scripts[i];
				if (script.path.empty())
				{
					continue;
				}

				const ActiveContextScope scope(*sceneCtx);
				UpdateScriptEntity(*host, *sceneCtx, e, static_cast<std::uint32_t>(i), script, dt);
			}
		}

		PurgeStaleInstances(world, *host, *sceneCtx);

		// Entity.Destroy from script: deferred to here so no callback ever
		// frees the component storage the loop above was iterating.
		if (!sceneCtx->pendingDestroys.empty())
		{
			for (const Entity doomed: std::exchange(sceneCtx->pendingDestroys, {}))
			{
				if (world.GetRegistry().valid(World::ToEntt(doomed)))
				{
					if (host->Language() == "rust")
					{
						// Match World::Destroy's complete subtree, including malformed cycles.
						// Detach while every owner still exists, so cleanup can use checked ECS APIs.
						std::vector<Entity> pending{doomed};
						std::unordered_set<std::uint32_t> subtree;
						while (!pending.empty())
						{
							const Entity current = pending.back();
							pending.pop_back();
							if (!world.GetRegistry().valid(World::ToEntt(current)) || !subtree.insert(current.id).second)
							{
								continue;
							}
							if (const auto* hierarchy = world.TryGet<HierarchyComponent>(current))
							{
								pending.insert(pending.end(), hierarchy->children.begin(), hierarchy->children.end());
							}
						}
						std::vector<std::uint64_t> destroyedKeys;
						for (const auto& [key, instance]: m_instances)
						{
							if (subtree.contains(InstanceEntityId(key)))
							{
								destroyedKeys.push_back(key);
							}
						}
						const ActiveContextScope scope(*sceneCtx);
						for (const auto key: destroyedKeys)
						{
							const auto found = m_instances.find(key);
							if (found == m_instances.end())
							{
								continue;
							}
							const auto handle = found->second.handle;
							(void) host->Detach(handle);
							(void) host->Destroy(handle);
							m_instances.erase(key);
						}
					}
					world.Destroy(doomed);
				}
			}
		}

		// Scene.Load from script: applied here, after every callback for the
		// frame has finished, because the switch destroys all entities.
		if (!sceneCtx->pendingSceneLoad.empty())
		{
			const std::string sceneName = std::exchange(sceneCtx->pendingSceneLoad, {});
			if (sceneCtx->beforeSceneLoad)
			{
				sceneCtx->beforeSceneLoad(sceneName);
			}
			// Persistent (DontDestroyOnLoad / SceneTransient) entities survive
			// the switch WITH their live script instances; everything else's
			// instances are torn down before their entities are.
			PruneInstancesForSceneSwitch(world, *host, *sceneCtx);
			if (scene::LoadSceneFile(sceneName, world, scene::MakeApplySceneDeps(m_services), scene::SceneLoadMode::GameplaySwitch))
			{
				if (auto* scenes = m_services.TryGet<SceneSubsystem>())
				{
					scenes->SetCurrentScene(sceneName);
				}
				AE_INFO(LogCategory::App, "Scene.Load: switched to '{}'", sceneName);
			}
			else
			{
				AE_WARN(LogCategory::App, "Scene.Load: scene '{}' not found - staying in the current scene", sceneName);
			}
		}
	}

	void ScriptComponentSystem::PruneInstancesForSceneSwitch(World& world, scripting::GameplayHost& host, scripting::SceneContext& ctx)
	{
		auto& reg = world.GetRegistry();
		// The OnDetach callbacks invoked below read ActiveContext() (e.g. CustomPass.Unregister ->
		// Registry() -> ActiveContext().services). Establish the active context first, exactly like the
		// sibling teardown helpers PurgeStaleInstances / DestroyAllInstances do - otherwise
		// g_activeContext is null here and any such OnDetach dereferences it and crashes.
		const ActiveContextScope scope(ctx);
		for (auto it = m_instances.begin(); it != m_instances.end();)
		{
			const Entity entity{InstanceEntityId(it->first)};
			const bool persistent = reg.valid(World::ToEntt(entity)) && ecs::HasDontDestroyOnLoadAncestor(world, entity);
			if (persistent)
			{
				++it;
				continue;
			}
			if (host.IsAvailable())
			{
				const scripting::ActiveScriptEntityScope entityScope(ctx, entity);
				(void) host.Detach(it->second.handle);
				(void) host.Destroy(it->second.handle);
			}
			it = m_instances.erase(it);
		}
	}

	void ScriptComponentSystem::Invalidate(World& world)
	{
		if (auto* host = m_services.TryGet<scripting::GameplayHost>())
		{
			if (auto* sceneCtx = m_services.TryGet<scripting::SceneContext>())
			{
				DestroyAllInstances(*host, *sceneCtx);
			}
		}
		m_instances.clear();
		m_failedTypes.clear();
		m_failedInstances.clear();

		for (auto&& [enttE, sc]: world.GetRegistry().view<ScriptComponent>().each())
		{
			for (ScriptEntry& script: sc.scripts)
			{
				script.attached = false;
			}
		}
		AE_INFO(LogCategory::App, "ScriptComponentSystem: handles invalidated (reload)");
	}
} // namespace aether::app
