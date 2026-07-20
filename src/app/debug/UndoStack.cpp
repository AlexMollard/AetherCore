#include "debug/UndoStack.hpp"

#include <utility>

#include "assets/AssetManager.hpp"
#include "rendering/Renderer.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	bool UndoStack::CaptureScene(World& world, ServiceContainer& services, app::scene::SceneDescription& outDesc, std::string& outKey) const
	{
		AE_PROFILE_ZONE();
		auto* assets = services.TryGet<AssetManager>();
		if (assets == nullptr)
		{
			return false;
		}
		outDesc = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), services.TryGet<Renderer>());
		outKey = app::scene::WriteToml(outDesc);
		return true;
	}

	void UndoStack::CaptureBaseline(World& world, ServiceContainer& services)
	{
		if (m_pendingBefore.has_value())
		{
			return; // already captured for this interaction
		}
		app::scene::SceneDescription desc;
		std::string key;
		if (!CaptureScene(world, services, desc, key))
		{
			return;
		}
		m_pendingBefore = std::move(desc);
		m_pendingBeforeKey = std::move(key);
	}

	void UndoStack::CommitPending(World& world, ServiceContainer& services)
	{
		if (!m_pendingBefore.has_value())
		{
			return;
		}
		app::scene::SceneDescription after;
		std::string afterKey;
		if (!CaptureScene(world, services, after, afterKey))
		{
			return;
		}
		if (afterKey == m_pendingBeforeKey)
		{
			// Nothing changed between baseline and commit (e.g. a bare click).
			m_pendingBefore.reset();
			m_pendingBeforeKey.clear();
			return;
		}
		auto diff = std::make_unique<EntityDiffCommand>(*m_pendingBefore, after);
		if (!diff->Empty())
		{
			RecordCommand(std::move(diff));
		}
		m_pendingBefore.reset();
		m_pendingBeforeKey.clear();
	}

	void UndoStack::AbandonPending()
	{
		m_pendingBefore.reset();
		m_pendingBeforeKey.clear();
	}

	void UndoStack::Record(std::unique_ptr<IEditorCommand> command)
	{
		if (command == nullptr)
		{
			return;
		}
		AbandonPending();
		RecordCommand(std::move(command));
	}

	void UndoStack::RecordCommand(std::unique_ptr<IEditorCommand> command)
	{
		m_undo.push_back(std::move(command));
		if (m_undo.size() > kMaxDepth)
		{
			m_undo.erase(m_undo.begin());
		}
		m_redo.clear();
	}

	IEditorCommand* UndoStack::Undo(World& world, ServiceContainer& services)
	{
		// A pending baseline means an edit was in flight; fold it in first so it is
		// not silently lost when the user undoes.
		CommitPending(world, services);
		if (m_undo.empty())
		{
			return nullptr;
		}
		std::unique_ptr<IEditorCommand> command = std::move(m_undo.back());
		m_undo.pop_back();
		command->Undo(world, services);
		IEditorCommand* raw = command.get();
		m_redo.push_back(std::move(command));
		if (m_redo.size() > kMaxDepth)
		{
			m_redo.erase(m_redo.begin());
		}
		return raw;
	}

	IEditorCommand* UndoStack::Redo(World& world, ServiceContainer& services)
	{
		CommitPending(world, services);
		if (m_redo.empty())
		{
			return nullptr;
		}
		std::unique_ptr<IEditorCommand> command = std::move(m_redo.back());
		m_redo.pop_back();
		command->Redo(world, services);
		IEditorCommand* raw = command.get();
		m_undo.push_back(std::move(command));
		if (m_undo.size() > kMaxDepth)
		{
			m_undo.erase(m_undo.begin());
		}
		return raw;
	}

	void UndoStack::Clear()
	{
		m_undo.clear();
		m_redo.clear();
		m_pendingBefore.reset();
		m_pendingBeforeKey.clear();
	}
} // namespace aether::editor
