#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "debug/EditorCommand.hpp"

namespace aether
{
	class ServiceContainer;
	class World;
} // namespace aether

namespace aether::editor
{
	// Command-history undo/redo. Every edit is recorded as a typed, reversible
	// command at the point of mutation and applied in place, so undo/redo preserve
	// entity ids and the current selection, and never destroy-and-recreate the whole
	// hierarchy. There is no scene-snapshot fallback: a mutation that records nothing
	// is not undoable, by design.
	class UndoStack
	{
	public:
		static constexpr std::size_t kMaxDepth = 64;

		// Record an already-applied typed command. Any coalesced field edit still in
		// flight is finalized first, so history stays in the order the user made it.
		void Record(std::unique_ptr<IEditorCommand> command);

		// Inspector field edits fire every frame a widget is active (one slider drag
		// is hundreds of calls), so they are coalesced instead of recorded per frame:
		// the first value seen for a field is kept as its "before", the latest as its
		// "after". Several (entity, component) targets can accumulate at once - one
		// drag on a multi-selection edits every selected entity - and all of them are
		// emitted together by FlushFieldEdit.
		void RecordFieldEdit(std::uint32_t entityId, const std::string& componentName, const std::string& field, const nlohmann::json& before, const nlohmann::json& after, bool isReflected);

		// Turn a coalesced field edit into one SetComponentCommand. Call when no
		// widget is active (i.e. the interaction finished).
		void FlushFieldEdit();

		// Drop an in-flight coalesced field edit without recording it. Used when the
		// editor leaves an editable state (entering compile/play), so a drag that was
		// mid-flight does not turn a later restore into a phantom edit.
		void AbandonPending();

		// Apply the top undo/redo command in place. Returns the command that ran
		// (so the caller can remap its selection through it), or nullptr.
		IEditorCommand* Undo(World& world, ServiceContainer& services);
		IEditorCommand* Redo(World& world, ServiceContainer& services);

		void Clear();

		[[nodiscard]] std::size_t UndoDepth() const
		{
			return m_undo.size();
		}

		[[nodiscard]] std::size_t RedoDepth() const
		{
			return m_redo.size();
		}

		// Unsaved-changes tracking. MarkSaved() pins the current history position as
		// the on-disk state; HasUnsavedChanges() is true after any later edit/undo/redo
		// until the next save or scene load (Clear). Intentionally conservative - it may
		// report changes after an undo returns to the saved content, but it never
		// reports "clean" while edits are outstanding, so a save/exit guard cannot
		// silently drop work.
		void MarkSaved()
		{
			m_savedSeq = m_editSeq;
		}

		[[nodiscard]] bool HasUnsavedChanges() const
		{
			return m_editSeq != m_savedSeq;
		}

		// Monotonic count of recorded edits. Autosave compares it against its own last
		// write so an idle-but-dirty session is not re-serialised every interval; it is
		// deliberately not the same question as HasUnsavedChanges, which tracks the last
		// real save.
		[[nodiscard]] std::uint64_t EditSequence() const
		{
			return m_editSeq;
		}

	private:
		void RecordCommand(std::unique_ptr<IEditorCommand> command);

		// In-flight inspector field edits, one entry per (entity, component) touched by
		// the current interaction (see RecordFieldEdit).
		struct PendingFieldEdit
		{
			std::uint32_t entityId = 0;
			std::string componentName;
			bool isReflected = true;
			nlohmann::json before;
			nlohmann::json after;
		};

		std::vector<PendingFieldEdit> m_pendingFields;

		std::vector<std::unique_ptr<IEditorCommand>> m_undo;
		std::vector<std::unique_ptr<IEditorCommand>> m_redo;

		// Monotonic count of scene-mutating operations (record/undo/redo); compared
		// against m_savedSeq to detect unsaved changes.
		std::size_t m_editSeq = 0;
		std::size_t m_savedSeq = 0;
	};

	// The open document was REPLACED - a new scene, a scene opened, a project opened.
	// History recorded against the previous scene addresses entity ids that no longer
	// exist, so replaying it would corrupt the new one; and the new document starts
	// clean, so the unsaved-changes guard must stop reporting the old scene's edits.
	//
	// Deliberately NOT called for Play/Stop or a gameplay scene switch: those restore
	// the same authored document, and resetting there would report a user's outstanding
	// edits as saved - the one thing this tracking must never do.
	void ResetEditHistory(ServiceContainer& services);
} // namespace aether::editor
