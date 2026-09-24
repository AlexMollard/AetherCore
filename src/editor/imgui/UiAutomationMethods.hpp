#pragma once

#include <vector>

#include "editor/ControlMethods.hpp"

namespace aether::editor
{
	// Appends the UI-automation control methods (ui.query / ui.click / ui.hover /
	// ui.input_text / ui.key) to a method table. Shared by the editor and the
	// launcher endpoints. Defined under src/app/imgui so both builds compile it
	// (the Launcher build excludes src/app/editor sources).
	void AppendUiAutomationMethods(std::vector<ControlMethod>& methods);
} // namespace aether::editor
