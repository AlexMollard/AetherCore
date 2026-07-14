#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"
#include "debug/EditorChrome.hpp"

namespace aether::editor
{
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
		void Apply();
		void Persist() const;
		bool LoadPersisted();

		chrome::EditorTheme m_theme = chrome::NightAmberTheme();
		bool m_loaded = false;
	};
} // namespace aether::editor
