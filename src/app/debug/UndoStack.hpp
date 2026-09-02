#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

		// Forget which components were claimed by commands recorded during the previous
		// frame. Call once before the Inspector snapshots its components: claims made while
		// this frame's drawers run then survive to this frame's diff pass, and no further.
		void ClearEditClaims();

		// Collect everything recorded until the matching EndGroup into ONE history entry, so a
		// request that edits many entities costs a single Ctrl+Z - the same way the hierarchy's
		// own multi-delete already behaves. Nests: only the outermost group emits.
		void BeginGroup(std::string label);
		void EndGroup();

		// Scoped BeginGroup/EndGroup, so an early return cannot leave the stack collecting.
		class ScopedGroup
		{
		public:
			ScopedGroup(UndoStack* stack, std::string label)
			    : m_stack(stack)
			{
				if (m_stack != nullptr)
				{
					m_stack->BeginGroup(std::move(label));
				}
			}
			~ScopedGroup()
			{
				if (m_stack != nullptr)
				{
					m_stack->EndGroup();
				}
			}
			ScopedGroup(const ScopedGroup&) = delete;
			ScopedGroup& operator=(const ScopedGroup&) = delete;
			ScopedGroup(ScopedGroup&&) = delete;
			ScopedGroup& operator=(ScopedGroup&&) = delete;

		private:
			UndoStack* m_stack = nullptr;
		};

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

		// Unsaved-changes tracking. MarkSaved() pins the current position in the history
		// as the on-disk state; HasUnsavedChanges() is true whenever the position differs
		// from it, and goes back to false if an undo returns to it.
		//
		// POSITION, not a count of operations. Counting made undo look like just another
		// edit, so a scene edited once and then undone read as dirty forever - which had
		// the autosave writing recovery copies holding no work, and the recovery prompt
		// offering them back on every project open.
		//
		// It still never reports clean while edits are outstanding. Undo then a DIFFERENT
		// edit lands on the same depth with different content, so the pin is dropped when
		// the branch holding it is discarded; see RecordCommand.
		void MarkSaved();

		// Drop the pin again. Used when a save that was optimistically marked clean turns
		// out to have failed on the writer thread: erring dirty costs a prompt, erring
		// clean costs the work.
		void MarkUnsaved();

		[[nodiscard]] bool HasUnsavedChanges() const
		{
			// A field edit mid-drag is already applied to the world but not yet recorded,
			// so the depth has not moved. Count it: erring dirty costs a prompt, erring
			// clean costs the work.
			if (!m_pendingFields.empty())
			{
				return true;
			}
			return !m_cleanDepth.has_value() || *m_cleanDepth != m_undo.size();
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

		// Non-zero while a group is open; only the outermost close emits an entry.
		int m_groupDepth = 0;
		std::string m_groupLabel;
		std::vector<std::unique_ptr<IEditorCommand>> m_grouped;

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
		// (entity, component) pairs a recorded command already accounts for this frame, so
		// the Inspector's post-draw diff does not record the same change again as a drag.
		std::vector<std::pair<std::uint32_t, std::string>> m_claimedEdits;

		std::vector<std::unique_ptr<IEditorCommand>> m_undo;
		std::vector<std::unique_ptr<IEditorCommand>> m_redo;

		// Monotonic count of scene-mutating operations (record/undo/redo). Autosave's
		// "has anything happened since my last write" question, which is not the same as
		// "does this differ from disk" - see HasUnsavedChanges.
		std::uint64_t m_editSeq = 0;

		// Undo depth at which the scene matches the file. Empty means the saved state is
		// no longer reachable by undoing (its branch was discarded, or it aged out of the
		// ring), so the document stays dirty until the next save.
		std::optional<std::size_t> m_cleanDepth = 0;
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
