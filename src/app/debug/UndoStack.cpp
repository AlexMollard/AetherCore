#include "debug/UndoStack.hpp"

#include "assets/AssetManager.hpp"
#include "rendering/Renderer.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	bool UndoStack::Capture(World& world, ServiceContainer& services, Entry& out)
	{
		AE_PROFILE_ZONE();
		auto* assets = services.TryGet<AssetManager>();
		if (assets == nullptr)
		{
			return false;
		}
		out.desc = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), services.TryGet<Renderer>());
		out.key = app::scene::WriteToml(out.desc);
		return true;
	}

	void UndoStack::Push(World& world, ServiceContainer& services)
	{
		Entry entry;
		if (!Capture(world, services, entry))
		{
			return;
		}
		if (!m_undo.empty() && m_undo.back().key == entry.key)
		{
			return;
		}
		m_undo.push_back(std::move(entry));
		if (m_undo.size() > kMaxDepth)
		{
			m_undo.erase(m_undo.begin());
		}
		m_redo.clear();
	}

	bool UndoStack::Undo(World& world, ServiceContainer& services)
	{
		Entry current;
		if (!Capture(world, services, current))
		{
			return false;
		}
		while (!m_undo.empty() && m_undo.back().key == current.key)
		{
			m_undo.pop_back();
		}
		if (m_undo.empty())
		{
			return false;
		}
		app::scene::ReplaceScene(m_undo.back().desc, world, app::scene::MakeApplySceneDeps(services));
		m_redo.push_back(std::move(current));
		if (m_redo.size() > kMaxDepth)
		{
			m_redo.erase(m_redo.begin());
		}
		m_undo.pop_back();
		return true;
	}

	bool UndoStack::Redo(World& world, ServiceContainer& services)
	{
		if (m_redo.empty())
		{
			return false;
		}
		Entry current;
		if (!Capture(world, services, current))
		{
			return false;
		}
		app::scene::ReplaceScene(m_redo.back().desc, world, app::scene::MakeApplySceneDeps(services));
		m_undo.push_back(std::move(current));
		if (m_undo.size() > kMaxDepth)
		{
			m_undo.erase(m_undo.begin());
		}
		m_redo.pop_back();
		return true;
	}
} // namespace aether::editor
