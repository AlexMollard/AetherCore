#include "debug/UndoStack.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "scene/World.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	void UndoStack::AbandonPending()
	{
		m_pendingFields.clear();
	}

	void UndoStack::Record(std::unique_ptr<IEditorCommand> command)
	{
		if (command == nullptr)
		{
			return;
		}
		// Finalize (not discard) any in-flight field edit so an interleaved command -
		// deleting an entity mid-drag, say - cannot swallow the drag that preceded it.
		FlushFieldEdit();
		RecordCommand(std::move(command));
	}

	void UndoStack::RecordFieldEdit(std::uint32_t entityId, const std::string& componentName, const std::string& field, const nlohmann::json& before, const nlohmann::json& after, bool isReflected)
	{
		auto it = std::find_if(m_pendingFields.begin(), m_pendingFields.end(), [&](const PendingFieldEdit& pending) { return pending.entityId == entityId && pending.componentName == componentName; });
		if (it == m_pendingFields.end())
		{
			PendingFieldEdit pending;
			pending.entityId = entityId;
			pending.componentName = componentName;
			pending.isReflected = isReflected;
			pending.before = nlohmann::json::object();
			pending.after = nlohmann::json::object();
			m_pendingFields.push_back(std::move(pending));
			it = std::prev(m_pendingFields.end());
		}
		// Earliest before per field wins; the latest after always replaces.
		if (!it->before.contains(field))
		{
			it->before[field] = before;
		}
		it->after[field] = after;
	}

	void UndoStack::FlushFieldEdit()
	{
		if (m_pendingFields.empty())
		{
			return;
		}
		std::vector<PendingFieldEdit> edits;
		edits.swap(m_pendingFields);
		for (PendingFieldEdit& edit: edits)
		{
			if (edit.before == edit.after)
			{
				continue; // dragged back to where it started - not an edit
			}
			// RecordCommand, not Record: Record() flushes first and would recurse.
			RecordCommand(std::make_unique<SetComponentCommand>(edit.entityId, edit.componentName, std::move(edit.before), std::move(edit.after), edit.isReflected));
		}
	}

	void UndoStack::RecordCommand(std::unique_ptr<IEditorCommand> command)
	{
		m_undo.push_back(std::move(command));
		if (m_undo.size() > kMaxDepth)
		{
			m_undo.erase(m_undo.begin());
		}
		m_redo.clear();
		++m_editSeq;
	}

	IEditorCommand* UndoStack::Undo(World& world, ServiceContainer& services)
	{
		// A coalesced field edit may still be in flight; fold it in first so it is
		// not silently lost when the user undoes.
		FlushFieldEdit();
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
		++m_editSeq;
		return raw;
	}

	IEditorCommand* UndoStack::Redo(World& world, ServiceContainer& services)
	{
		FlushFieldEdit();
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
		++m_editSeq;
		return raw;
	}

	void UndoStack::Clear()
	{
		m_undo.clear();
		m_redo.clear();
		m_pendingFields.clear();
		// A fresh scene load starts clean.
		m_editSeq = 0;
		m_savedSeq = 0;
	}

	void ResetEditHistory(ServiceContainer& services)
	{
		if (auto* undo = services.TryGet<UndoStack>())
		{
			undo->Clear();
		}
	}
} // namespace aether::editor
