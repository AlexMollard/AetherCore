#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"
#include "debug/EditorChrome.hpp"

namespace aether::editor
{
	// Live editor theme configurator. Edits the runtime chrome palette
	// (chrome::EditorTheme) - accent, surfaces, text tiers, semantic colours - and
	// applies changes across every panel + the ImGui widget style immediately.
	// Named presets ship a few alternatives to the default "Night Amber". The
	// active theme persists to EditorTheme.toml in the user config dir and is
	// reloaded on the next launch.
	class ThemePanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Theme";
		}

		void OnAttach(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;

	private:
		void Apply();          // push m_theme into chrome + restyle ImGui
		void Persist() const;  // write m_theme to EditorTheme.toml
		bool LoadPersisted();  // read EditorTheme.toml into m_theme; false if none

		chrome::EditorTheme m_theme = chrome::NightAmberTheme();
		bool m_loaded = false;
	};
} // namespace aether::editor
