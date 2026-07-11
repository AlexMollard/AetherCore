#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::app
{
	struct EditorWindowInfo
	{
		std::string name; // the panel's display name (what ImGui titles the window)
		bool visible = false;
	};

	// Editor window control exposed to non-layer code (the control endpoint / MCP)
	// via the ServiceContainer, mirroring EditorProjectActions. DebugLayer registers
	// this in OnAttach and clears it in OnDetach, so the callbacks never outlive the
	// panels they capture. Handlers run on the main thread (the control server drains
	// its queue there), the same thread that draws the panels - so toggling
	// visibility here is race-free.
	struct EditorWindowActions
	{
		// Every editor panel with its current visibility.
		std::function<std::vector<EditorWindowInfo>()> list;
		// Show/hide a panel by display name (case-insensitive). Returns false if no
		// panel matches.
		std::function<bool(std::string_view name, bool visible)> setVisible;
		// Ensure the Inspector is open and scroll the named component's drawer into
		// view (force-opening it) on the next frame - e.g. "Rigid Body". Matches the
		// section label case-insensitively.
		std::function<void(std::string_view component)> focusInspectorComponent;
	};
} // namespace aether::app
