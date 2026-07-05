#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "scene/SceneSerializer.hpp"

namespace aether
{
	class ServiceContainer;
	class World;
} // namespace aether

namespace aether::app
{
	// Snapshot-based editor undo. Push() captures the whole edit-mode scene
	// (cheap at editor scale) BEFORE a gesture mutates it; Undo/Redo restore
	// through the same transient-sparing ReplaceScene the Stop button uses -
	// so the scripted player and other runtime actors survive, exactly like
	// Stop. Entries dedup against the stack top by serialized content, which
	// makes Push safe to call speculatively: the debug layer pushes on EVERY
	// left-mouse press while editing, coalescing a whole drag (gizmo, slider)
	// or click (palette, delete button) into one undo step. Keyboard-driven
	// edits (Ctrl+D/V/X, Delete) push explicitly at their sites.
	//
	// Known v1 limits: restores only what the scene format serializes (das
	// session globals and cameras are untouched), and selection clears on
	// undo/redo since entity ids are rebuilt.
	class UndoStack
	{
	public:
		static constexpr std::size_t kMaxDepth = 32;

		// Record the current scene as an undo point (no-op when it matches
		// the top entry or when capture deps are unavailable). Clears redo.
		void Push(World& world, ServiceContainer& services);

		bool Undo(World& world, ServiceContainer& services);
		bool Redo(World& world, ServiceContainer& services);

		[[nodiscard]] std::size_t UndoDepth() const
		{
			return m_undo.size();
		}

		[[nodiscard]] std::size_t RedoDepth() const
		{
			return m_redo.size();
		}

	private:
		struct Entry
		{
			scene::SceneDescription desc;
			std::string key; // serialized TOML: cheap, exact change detection
		};

		bool Capture(World& world, ServiceContainer& services, Entry& out);

		std::vector<Entry> m_undo;
		std::vector<Entry> m_redo;
	};
} // namespace aether::app
