#include "systems/ScriptComponentSystem.hpp"

#include <vector>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
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

		// Installs the active SceneContext for the duration of a managed call
		// group, so the C# exports (which read scripting::ActiveContext()) resolve.
		struct ActiveContextScope
		{
			explicit ActiveContextScope(scripting::SceneContext& ctx)
			{
				scripting::g_activeContext = &ctx;
			}

			~ActiveContextScope()
			{
				scripting::g_activeContext = nullptr;
			}

			ActiveContextScope(const ActiveContextScope&) = delete;
			ActiveContextScope& operator=(const ActiveContextScope&) = delete;
		};
	} // namespace

	ScriptComponentSystem::~ScriptComponentSystem()
	{
		// Managed GCHandles are intentionally not freed here: at shutdown the host
		// may already be gone and CoreCLR never unloads, so leaking them is inert.
		m_instances.clear();
	}

	std::uint64_t ScriptComponentSystem::GetInstanceHandle(std::uint32_t entityId, std::uint32_t scriptIndex) const
	{
		const auto it = m_instances.find(InstanceKey(entityId, scriptIndex));
		return it != m_instances.end() ? it->second.handle : 0;
	}

	bool ScriptComponentSystem::UpdateCSharpEntity(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& /*ctx*/, Entity entity, std::uint32_t scriptIndex, ScriptEntry& script, float dt)
	{
		const auto* api = cs.Api();
		if (api == nullptr)
		{
			return false;
		}
		const std::string& typeName = script.path;
		if (m_failedTypes.contains(typeName))
		{
			return false;
		}

		const std::uint64_t key = InstanceKey(entity.id, scriptIndex);
		std::uint64_t handle = 0;
		if (const auto it = m_instances.find(key); it != m_instances.end())
		{
			handle = it->second.handle;
		}

		// Re-play / re-attach (scene apply reset `attached`): discard the previous
		// instance so the script restarts from fresh per-entity state.
		if (!script.attached && handle != 0)
		{
			if (api->InvokeDetach != nullptr)
			{
				api->InvokeDetach(handle);
			}
			if (api->DestroyInstance != nullptr)
			{
				api->DestroyInstance(handle);
			}
			m_instances.erase(key);
			handle = 0;
		}

		if (handle == 0)
		{
			handle = api->CreateInstance != nullptr ? api->CreateInstance(typeName.c_str(), entity.id) : 0;
			if (handle == 0)
			{
				AE_WARN(LogCategory::App, "ScriptComponent: C# type '{}' failed to instantiate - disabled until reload", typeName);
				m_failedTypes.insert(typeName);
				return false;
			}
			m_instances[key] = Instance{.handle = handle, .typeName = typeName};
			// Apply serialized field overrides before OnAttach sees them.
			cs.ApplyProperties(handle, typeName, script.properties);
			script.attached = false; // a freshly created instance must attach
		}

		if (!script.attached)
		{
			// Flag first for parity with the das runner's no-retry contract
			// (managed OnAttach also guards its own exceptions).
			script.attached = true;
			if (api->InvokeAttach != nullptr)
			{
				api->InvokeAttach(handle);
			}
		}

		if (api->InvokeUpdate != nullptr)
		{
			api->InvokeUpdate(handle, dt);
		}
		return true;
	}

	void ScriptComponentSystem::PurgeStaleCSharpInstances(World& world, scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx)
	{
		const auto* api = cs.Api();
		if (api == nullptr || m_instances.empty())
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

		ActiveContextScope scope(ctx);
		for (const std::uint64_t key: stale)
		{
			const std::uint64_t handle = m_instances[key].handle;
			if (api->InvokeDetach != nullptr)
			{
				api->InvokeDetach(handle);
			}
			if (api->DestroyInstance != nullptr)
			{
				api->DestroyInstance(handle);
			}
			m_instances.erase(key);
		}
	}

	void ScriptComponentSystem::DestroyAllCSharpInstances(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx)
	{
		const auto* api = cs.Api();
		if (api == nullptr || m_instances.empty())
		{
			m_instances.clear();
			return;
		}

		ActiveContextScope scope(ctx);
		for (const auto& [key, instance]: m_instances)
		{
			if (api->InvokeDetach != nullptr)
			{
				api->InvokeDetach(instance.handle);
			}
			if (api->DestroyInstance != nullptr)
			{
				api->DestroyInstance(instance.handle);
			}
		}
		m_instances.clear();
	}

	void ScriptComponentSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		auto* sceneCtx = m_services.TryGet<scripting::SceneContext>();
		if (sceneCtx == nullptr)
		{
			return;
		}
		auto* csScripting = m_services.TryGet<scripting::CSharpScriptingSubsystem>();
		if (csScripting == nullptr || !csScripting->IsAvailable())
		{
			return;
		}

		// Keep delta current for Input.DeltaTime.
		sceneCtx->deltaTime = dt;

		// Snapshot first: scripts may create/destroy entities (spawns) which
		// would invalidate a live view iteration.
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
				continue; // destroyed by an earlier script this tick
			}
			if (ecs::HasDisabledAncestor(world, e))
			{
				continue; // disabled entities (and subtrees) don't tick scripts
			}
			auto* sc = world.TryGet<ScriptComponent>(e);
			if (sc == nullptr || sc->scripts.empty())
			{
				continue;
			}

			for (std::size_t i = 0; i < sc->scripts.size(); ++i)
			{
				ScriptEntry& script = sc->scripts[i];
				if (script.path.empty())
				{
					continue;
				}

				ActiveContextScope scope(*sceneCtx);
				UpdateCSharpEntity(*csScripting, *sceneCtx, e, static_cast<std::uint32_t>(i), script, dt);
			}
		}

		// Detach C# instances whose entity/component went away this frame.
		PurgeStaleCSharpInstances(world, *csScripting, *sceneCtx);
	}

	void ScriptComponentSystem::Invalidate(World& world)
	{
		// Tear down all live C# instances so a reload starts fresh.
		if (auto* csScripting = m_services.TryGet<scripting::CSharpScriptingSubsystem>())
		{
			if (auto* sceneCtx = m_services.TryGet<scripting::SceneContext>())
			{
				DestroyAllCSharpInstances(*csScripting, *sceneCtx);
			}
		}
		m_instances.clear();
		m_failedTypes.clear();

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
