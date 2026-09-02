#pragma once

#include <span>

#include <imgui.h>

namespace aether::editor::shortcuts
{
	// The editor's key bindings, in one place.
	//
	// Both the code that HANDLES a shortcut and the F1 reference that DOCUMENTS it read from
	// here, so the two cannot disagree. They previously could: the bindings live in three
	// different panels and the reference was a separate hand-written list, which is a
	// documentation drift waiting to happen.
	//
	// This deliberately carries only the key. Each call site keeps its own guards - whether
	// the panel is focused, whether the game has the keyboard, whether the pointer is over
	// the viewport - because those differ per site and are not a property of the binding.
	enum class Context
	{
		Global,
		Scene,
		Viewport,
	};

	struct Binding
	{
		ImGuiKey key;
		// How it is written down for the user, including modifiers the call site tests.
		const char* display;
		const char* description;
		Context context;
	};

	inline constexpr Binding kCommandPalette{ImGuiKey_P, "Ctrl+P", "Command palette", Context::Global};
	inline constexpr Binding kSave{ImGuiKey_S, "Ctrl+S", "Save the scene, or the focused material", Context::Global};
	inline constexpr Binding kUndo{ImGuiKey_Z, "Ctrl+Z", "Undo", Context::Global};
	inline constexpr Binding kRedo{ImGuiKey_Y, "Ctrl+Y  /  Ctrl+Shift+Z", "Redo", Context::Global};
	inline constexpr Binding kShortcuts{ImGuiKey_F1, "F1", "This list", Context::Global};
	inline constexpr Binding kReloadScripts{ImGuiKey_F5, "F5", "Reload C# scripts", Context::Global};
	inline constexpr Binding kPausePlay{ImGuiKey_F6, "F6", "Pause / resume play", Context::Global};
	inline constexpr Binding kStepFrame{ImGuiKey_F7, "F7", "Step one frame", Context::Global};

	inline constexpr Binding kCopy{ImGuiKey_C, "Ctrl+C", "Copy the selection", Context::Scene};
	inline constexpr Binding kCut{ImGuiKey_X, "Ctrl+X", "Cut the selection", Context::Scene};
	inline constexpr Binding kPaste{ImGuiKey_V, "Ctrl+V", "Paste", Context::Scene};
	inline constexpr Binding kDuplicate{ImGuiKey_D, "Ctrl+D", "Duplicate", Context::Scene};
	inline constexpr Binding kGroup{ImGuiKey_G, "Ctrl+G", "Group the selection under a new parent", Context::Scene};
	inline constexpr Binding kRename{ImGuiKey_F2, "F2", "Rename", Context::Scene};
	inline constexpr Binding kDelete{ImGuiKey_Delete, "Delete", "Delete the selection", Context::Scene};

	inline constexpr Binding kGizmoMove{ImGuiKey_W, "W", "Move gizmo", Context::Viewport};
	inline constexpr Binding kGizmoRotate{ImGuiKey_E, "E", "Rotate gizmo", Context::Viewport};
	inline constexpr Binding kGizmoScale{ImGuiKey_R, "R", "Scale gizmo", Context::Viewport};
	inline constexpr Binding kFrameSelection{ImGuiKey_F, "F", "Frame the selection", Context::Viewport};

	// Everything above, for the reference to render. A binding missing from here is a binding
	// nobody can discover, so add to this when you add one.
	inline constexpr Binding kAll[] = {
	        kCommandPalette, kSave, kUndo, kRedo, kShortcuts, kReloadScripts, kPausePlay, kStepFrame,
	        kCopy, kCut, kPaste, kDuplicate, kGroup, kRename, kDelete,
	        kGizmoMove, kGizmoRotate, kGizmoScale, kFrameSelection};

	[[nodiscard]] inline std::span<const Binding> All()
	{
		return {kAll, sizeof(kAll) / sizeof(kAll[0])};
	}
} // namespace aether::editor::shortcuts
