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
