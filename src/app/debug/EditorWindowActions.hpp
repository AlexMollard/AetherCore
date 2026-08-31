#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::editor
{
	struct EditorWindowInfo
	{
		std::string name;
		bool visible = false;
	};

	// this in OnAttach and clears it in OnDetach, so the callbacks never outlive the
	struct EditorWindowActions
	{
		std::function<std::vector<EditorWindowInfo>()> list;
		std::function<bool(std::string_view name, bool visible)> setVisible;
		// Bring a panel to the front of whatever dock node it sits in. Visibility alone is not
		// enough to see one: a panel sharing a node with others is "visible" while its tab is
		// behind theirs, which looks exactly like a panel that draws nothing.
		std::function<bool(std::string_view name)> focusWindow;
		// Queue one undo (redo=false) or redo (redo=true) step. It is applied on the next
		// frame at the same point as the Ctrl+Z / Ctrl+Y shortcut, so both share the same
		// play-mode gate and selection remapping - the effect shows in the following
		// undo_status, not in the call that queued it.
		std::function<void(bool redo)> historyStep;
		std::function<void(std::string_view component)> focusInspectorComponent;
		// Opens the File > Open scene dialog (used by the menu and the control
		// endpoint alike).
		std::function<void()> openSceneDialog;
		// Built-in workflow dock layouts: list their names, and apply one by name
		// (case-insensitive). Used by Window > Layouts and the control endpoint.
		std::function<std::vector<std::string>()> listLayouts;
		std::function<bool(std::string_view name)> applyLayout;
	};
} // namespace aether::editor
