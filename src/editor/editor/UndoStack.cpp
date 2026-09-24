#include "editor/UndoStack.hpp"

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

	void UndoStack::ClearEditClaims()
	{
		m_claimedEdits.clear();
	}

	void UndoStack::Record(std::unique_ptr<IEditorCommand> command)
	{
		if (command != nullptr)
		{
			// Claim before anything else: a grouped command still has to suppress the
			// Inspector's diff, and an early return below must not skip the claim.
			if (const IEditorCommand::EditTarget target = command->Target(); !target.component.empty())
			{
				m_claimedEdits.emplace_back(target.entityId, std::string(target.component));
			}
		}
		if (m_groupDepth > 0)
		{
			if (command != nullptr)
			{
				m_grouped.push_back(std::move(command));
			}
			return;
		}
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
		// A command recorded during this frame's drawing already accounts for this
		// component; the Inspector's post-draw diff sees the same change and would record it
		// a second time as a drag the user never made.
		if (std::any_of(m_claimedEdits.begin(), m_claimedEdits.end(),
		        [&](const std::pair<std::uint32_t, std::string>& claim) { return claim.first == entityId && claim.second == componentName; }))
		{
			return;
		}
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
		std::vector<std::unique_ptr<IEditorCommand>> commands;
		for (PendingFieldEdit& edit: edits)
		{
			if (edit.before == edit.after)
			{
				continue; // dragged back to where it started - not an edit
			}
			commands.push_back(std::make_unique<SetComponentCommand>(edit.entityId, edit.componentName, std::move(edit.before), std::move(edit.after), edit.isReflected));
		}
		if (commands.empty())
		{
			return;
		}
		// One gesture, one history entry. A drag over a multi-selection lands here with a
		// command per (entity, component); recorded singly the user would undo the same
		// drag once per target, seeing the selection half-reverted along the way.
		// RecordCommand, not Record: Record() flushes first and would recurse.
		if (commands.size() == 1)
		{
			RecordCommand(std::move(commands.front()));
			return;
		}
		RecordCommand(std::make_unique<CompositeCommand>(std::move(commands), "Set components"));
	}

	void UndoStack::MarkSaved()
	{
		// A drag in flight is already in the world, so it is in the capture this pin is
		// for. Fold it into history first or the pin lands one command short and the
		// scene reads dirty the moment the drag ends.
		FlushFieldEdit();
		m_cleanDepth = m_undo.size();
	}

	void UndoStack::BeginGroup(std::string label)
	{
		if (m_groupDepth == 0)
		{
			// A drag still in flight belongs to what came before the group, not inside it.
			FlushFieldEdit();
			m_groupLabel = std::move(label);
			m_grouped.clear();
		}
		++m_groupDepth;
	}

	void UndoStack::EndGroup()
	{
		if (m_groupDepth == 0)
		{
			return;
		}
		--m_groupDepth;
		if (m_groupDepth > 0)
		{
			return;
		}
		std::vector<std::unique_ptr<IEditorCommand>> commands;
		commands.swap(m_grouped);
		if (commands.empty())
		{
			return;
		}
		if (commands.size() == 1)
		{
			RecordCommand(std::move(commands.front()));
			return;
		}
		RecordCommand(std::make_unique<CompositeCommand>(std::move(commands), m_groupLabel));
	}

	void UndoStack::RecordCommand(std::unique_ptr<IEditorCommand> command)
	{
		// A new edit discards the redo branch. If the saved state lived in there it can no
		// longer be reached by undoing, so the pin has to go: undo, then a DIFFERENT edit,
		// lands back on the saved DEPTH holding different content, and that is the one way
		// a position-based check could report clean over unsaved work.
		if (m_cleanDepth.has_value() && *m_cleanDepth > m_undo.size())
		{
			m_cleanDepth.reset();
		}

		m_undo.push_back(std::move(command));
		if (m_undo.size() > kMaxDepth)
		{
			m_undo.erase(m_undo.begin());
			// Dropping the oldest command shifts every depth down one. A pin at 0 was the
			// state before that command, which is no longer reachable at all.
			if (m_cleanDepth.has_value())
			{
				if (*m_cleanDepth == 0)
				{
					m_cleanDepth.reset();
				}
				else
				{
					--*m_cleanDepth;
				}
			}
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

	void ResetEditHistory(ServiceContainer& services)
	{
		if (auto* undo = services.TryGet<UndoStack>())
		{
			undo->Clear();
		}
	}
} // namespace aether::editor
