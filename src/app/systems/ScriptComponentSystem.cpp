#include "systems/ScriptComponentSystem.hpp"

#include <utility>
#include <vector>

#include "IEngineRuntime.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scene/World.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
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
	} // namespace

	ScriptComponentSystem::~ScriptComponentSystem()
	{
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
			cs.ApplyProperties(handle, typeName, script.properties);
			script.attached = false; // a freshly created instance must attach
		}

		if (!script.attached)
		{
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

		const ActiveContextScope scope(ctx);
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

		const ActiveContextScope scope(ctx);
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

				const ActiveContextScope scope(*sceneCtx);
				UpdateCSharpEntity(*csScripting, *sceneCtx, e, static_cast<std::uint32_t>(i), script, dt);
			}
		}

		PurgeStaleCSharpInstances(world, *csScripting, *sceneCtx);

		// Entity.Destroy from script: deferred to here so no callback ever
		// frees the component storage the loop above was iterating.
		if (!sceneCtx->pendingDestroys.empty())
		{
			for (const Entity doomed: std::exchange(sceneCtx->pendingDestroys, {}))
			{
				if (world.GetRegistry().valid(World::ToEntt(doomed)))
				{
					world.Destroy(doomed);
				}
			}
		}

		// Scene.Load from script: applied here, after every callback for the
		// frame has finished, because the switch destroys all entities.
		if (!sceneCtx->pendingSceneLoad.empty())
		{
			const std::string sceneName = std::exchange(sceneCtx->pendingSceneLoad, {});
			// Persistent (DontDestroyOnLoad / SceneTransient) entities survive
			// the switch WITH their live script instances; everything else's
			// instances are torn down before their entities are.
			PruneInstancesForSceneSwitch(world, *csScripting, *sceneCtx);
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

	void ScriptComponentSystem::PruneInstancesForSceneSwitch(World& world, scripting::CSharpScriptingSubsystem& cs, scripting::SceneContext& ctx)
	{
		const auto* api = cs.Api();
		auto& reg = world.GetRegistry();
		// The OnDetach callbacks invoked below read ActiveContext() (e.g. CustomPass.Unregister ->
		// Registry() -> ActiveContext().services). Establish the active context first, exactly like the
		// sibling teardown helpers PurgeStaleCSharpInstances / DestroyAllCSharpInstances do - otherwise
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
			if (api != nullptr)
			{
				if (api->InvokeDetach != nullptr)
				{
					api->InvokeDetach(it->second.handle);
				}
				if (api->DestroyInstance != nullptr)
				{
					api->DestroyInstance(it->second.handle);
				}
			}
			it = m_instances.erase(it);
		}
	}

	void ScriptComponentSystem::Invalidate(World& world)
	{
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
