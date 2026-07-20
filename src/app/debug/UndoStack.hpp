#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "debug/EditorCommand.hpp"
#include "scene/SceneSerializer.hpp"

namespace aether
{
	class ServiceContainer;
	class World;
} // namespace aether

namespace aether::editor
{
	// Command-history undo/redo. Edits are captured as reversible commands and
	// applied in place, so undo/redo preserve entity ids and the current
	// selection, and never destroy-and-recreate the whole hierarchy.
	//
	// Coarse capture model for edits without a dedicated typed command:
	//   - CaptureBaseline() snapshots the pre-edit scene once per interaction.
	//   - CommitPending() snapshots the post-edit scene and, if it differs,
	//     records a surgical EntityDiffCommand(before, after). Call it at end of
	//     frame when the mouse is up, so a multi-frame drag becomes one command.
	class UndoStack
	{
	public:
		static constexpr std::size_t kMaxDepth = 64;

		// Snapshot the current scene as the baseline for the edit about to happen.
		// No-op if a baseline is already pending this interaction.
		void CaptureBaseline(World& world, ServiceContainer& services);

		// Back-compat alias for existing edit sites that snapshot before mutating.
		void Push(World& world, ServiceContainer& services)
		{
			CaptureBaseline(world, services);
		}

		// Record an already-applied typed command directly (bypassing the coarse
		// baseline/commit path). Drops any pending baseline so a site that records a
		// precise command doesn't also emit a redundant generic one.
		void Record(std::unique_ptr<IEditorCommand> command);

		// Finalize a pending baseline: if the scene changed, record a command.
		void CommitPending(World& world, ServiceContainer& services);

		// Drop an in-flight baseline without recording anything. Used when the
		// editor leaves an editable state (entering compile/play), so a baseline
		// captured just before does not turn a later restore into a phantom edit.
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

	private:
		[[nodiscard]] bool CaptureScene(World& world, ServiceContainer& services, app::scene::SceneDescription& outDesc, std::string& outKey) const;
		void RecordCommand(std::unique_ptr<IEditorCommand> command);

		std::optional<app::scene::SceneDescription> m_pendingBefore;
		std::string m_pendingBeforeKey;

		std::vector<std::unique_ptr<IEditorCommand>> m_undo;
		std::vector<std::unique_ptr<IEditorCommand>> m_redo;

		// Monotonic count of scene-mutating operations (record/undo/redo); compared
		// against m_savedSeq to detect unsaved changes.
		std::size_t m_editSeq = 0;
		std::size_t m_savedSeq = 0;
	};
} // namespace aether::editor
