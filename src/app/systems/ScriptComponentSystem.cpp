#include "systems/ScriptComponentSystem.hpp"

#include <vector>

#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/ScriptingSubsystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app
{
	ScriptComponentSystem::~ScriptComponentSystem()
	{
		for (auto& [path, handle]: m_handles)
		{
			scripting::ScriptingSubsystem::FreeHandle(handle);
		}
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

	void ScriptComponentSystem::Update(World& world, float dt)
	{
		AE_PROFILE_ZONE();
		auto* sceneCtx = m_services.TryGet<scripting::SceneContext>();
		auto* scripting = m_services.TryGet<scripting::ScriptingSubsystem>();
		if (sceneCtx == nullptr || scripting == nullptr)
		{
			return;
		}

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
			scripting::ScriptHandle* handle = HandleFor(sc->path);
			if (handle == nullptr)
			{
				continue;
			}
			if (!sc->attached)
			{
				// Flag first: a throwing attach must not retry every frame.
				sc->attached = true;
				scripting->CallEntityAttach(*handle, *sceneCtx, e.id);
			}
			scripting->CallEntityUpdate(*handle, *sceneCtx, e.id, dt);
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
		for (auto&& [enttE, sc]: world.GetRegistry().view<ScriptComponent>().each())
		{
			sc.attached = false;
		}
		AE_INFO(LogCategory::App, "ScriptComponentSystem: handles invalidated (reload)");
	}
} // namespace aether::app
