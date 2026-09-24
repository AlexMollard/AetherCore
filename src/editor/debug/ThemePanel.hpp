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

	// Reads the user's persisted theme (Theme panel > save) into out. Returns false when
	// nothing is persisted, leaving out untouched. Shared with the flavor machinery so a
	// non-flavored project reopens with exactly the theme the user last chose.
	bool LoadPersistedEditorTheme(chrome::EditorTheme& out);
} // namespace aether::editor
