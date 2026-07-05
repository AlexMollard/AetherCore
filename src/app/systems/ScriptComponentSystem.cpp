#include "systems/ScriptComponentSystem.hpp"

#include <vector>

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app
{
	namespace
	{
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

		[[nodiscard]] bool IsDasPath(const std::string& path)
		{
			return path.ends_with(".das");
		}
	} // namespace

	ScriptComponentSystem::~ScriptComponentSystem()
	{
		for (auto& [path, handle]: m_handles)
		{
			scripting::ScriptingSubsystem::FreeHandle(handle);
		}
		// Managed GCHandles are intentionally not freed here: at shutdown the host
		// may already be gone and CoreCLR never unloads, so leaking them is inert.
		m_instances.clear();
	}

	scripting::ScriptHandle* ScriptComponentSystem::HandleFor(const std::string& path)
	{
		if (const auto it = m_handles.find(path); it != m_handles.end())
		{
			return &it->second;
		}
		if (m_failed.contains(path))
		{
			return nullptr;
		}
		auto* scripting = m_services.TryGet<scripting::ScriptingSubsystem>();
		if (scripting == nullptr)
		{
			return nullptr;
		}
		scripting::ScriptHandle handle = scripting->Compile(path);
		if (!handle.IsValid())
		{
			AE_WARN(LogCategory::App, "ScriptComponent: '{}' failed to compile - disabled until the next reload", path);
			m_failed.insert(path);
			return nullptr;
		}
		if (handle.onEntityAttach == nullptr && handle.onEntityUpdate == nullptr)
		{
			AE_WARN(LogCategory::App, "ScriptComponent: '{}' exports neither on_entity_attach nor on_entity_update", path);
		}
		return &m_handles.emplace(path, std::move(handle)).first->second;
	}

	std::uint64_t ScriptComponentSystem::GetInstanceHandle(std::uint32_t entityId) const
	{
		const auto it = m_instances.find(entityId);
		return it != m_instances.end() ? it->second : 0;
	}

	bool ScriptComponentSystem::UpdateCSharpEntity(scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& /*ctx*/,
		Entity entity, const std::string& typeName, const std::map<std::string, ScriptPropertyValue>& properties, bool& attached,
		float dt)
	{
		const auto* api = cs.Api();
		if (api == nullptr)
		{
			return false;
		}
		if (m_failedTypes.contains(typeName))
		{
			return false;
		}

		std::uint64_t handle = 0;
		if (const auto it = m_instances.find(entity.id); it != m_instances.end())
		{
			handle = it->second;
		}

		// Re-play / re-attach (scene apply reset `attached`): discard the previous
		// instance so the script restarts from fresh per-entity state.
		if (!attached && handle != 0)
		{
			if (api->InvokeDetach != nullptr)
			{
				api->InvokeDetach(handle);
			}
			if (api->DestroyInstance != nullptr)
			{
				api->DestroyInstance(handle);
			}
			m_instances.erase(entity.id);
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
			m_instances[entity.id] = handle;
			// Apply serialized field overrides before OnAttach sees them.
			cs.ApplyProperties(handle, typeName, properties);
			attached = false; // a freshly created instance must attach
		}

		if (!attached)
		{
			// Flag first for parity with the das runner's no-retry contract
			// (managed OnAttach also guards its own exceptions).
			attached = true;
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

	void ScriptComponentSystem::PurgeStaleCSharpInstances(World& world, scripting::CSharpScriptingSubsystem& cs,
		scripting::SceneContext& ctx)
	{
		const auto* api = cs.Api();
		if (api == nullptr || m_instances.empty())
		{
			return;
		}

		auto& reg = world.GetRegistry();
		std::vector<std::uint32_t> stale;
		for (const auto& [id, handle]: m_instances)
		{
			const auto enttE = World::ToEntt(Entity{id});
			const bool alive = reg.valid(enttE);
			const auto* sc = alive ? world.TryGet<ScriptComponent>(Entity{id}) : nullptr;
			const bool stillCSharp = sc != nullptr && !sc->path.empty() && !IsDasPath(sc->path);
			if (!stillCSharp)
			{
				stale.push_back(id);
			}
		}

		if (stale.empty())
		{
			return;
		}

		ActiveContextScope scope(ctx);
		for (const std::uint32_t id: stale)
		{
			const std::uint64_t handle = m_instances[id];
			if (api->InvokeDetach != nullptr)
			{
				api->InvokeDetach(handle);
			}
			if (api->DestroyInstance != nullptr)
			{
				api->DestroyInstance(handle);
			}
			m_instances.erase(id);
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
		for (const auto& [id, handle]: m_instances)
		{
			if (api->InvokeDetach != nullptr)
			{
				api->InvokeDetach(handle);
			}
			if (api->DestroyInstance != nullptr)
			{
				api->DestroyInstance(handle);
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
		auto* dasScripting = m_services.TryGet<scripting::ScriptingSubsystem>();
		auto* csScripting = m_services.TryGet<scripting::CSharpScriptingSubsystem>();

		// Keep delta current for Input.DeltaTime / get_delta_time.
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
			auto* sc = world.TryGet<ScriptComponent>(e);
			if (sc == nullptr || sc->path.empty())
			{
				continue;
			}

			if (IsDasPath(sc->path))
			{
				if (dasScripting == nullptr)
				{
					continue;
				}
				scripting::ScriptHandle* handle = HandleFor(sc->path);
				if (handle == nullptr)
				{
					continue;
				}
				if (!sc->attached)
				{
					// Flag first: a throwing attach must not retry every frame.
					sc->attached = true;
					dasScripting->CallEntityAttach(*handle, *sceneCtx, e.id);
				}
				dasScripting->CallEntityUpdate(*handle, *sceneCtx, e.id, dt);
			}
			else
			{
				if (csScripting == nullptr || !csScripting->IsAvailable())
				{
					continue;
				}
				ActiveContextScope scope(*sceneCtx);
				UpdateCSharpEntity(*csScripting, *sceneCtx, e, sc->path, sc->properties, sc->attached, dt);
			}
		}

		// Detach C# instances whose entity/component went away this frame.
		if (csScripting != nullptr && csScripting->IsAvailable())
		{
			PurgeStaleCSharpInstances(world, *csScripting, *sceneCtx);
		}
	}

	void ScriptComponentSystem::Invalidate(World& world)
	{
		for (auto& [path, handle]: m_handles)
		{
			scripting::ScriptingSubsystem::FreeHandle(handle);
		}
		m_handles.clear();
		m_failed.clear();

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
			sc.attached = false;
		}
		AE_INFO(LogCategory::App, "ScriptComponentSystem: handles invalidated (reload)");
	}
} // namespace aether::app
