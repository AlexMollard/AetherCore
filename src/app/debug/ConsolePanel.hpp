#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class ConsolePanel final : public DebugPanel
	{
	public:
		[[nodiscard]] std::string_view GetName() const override
		{
			return "Console";
		}

		// Part of the default layout: where script errors and Log.Info land.
		[[nodiscard]] bool DefaultVisible() const override
		{
			return true;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		char m_filter[128] = {};
		bool m_showVerbose = false;
		bool m_showInfo = true;
		bool m_showWarn = true;
		bool m_showError = true;
		bool m_autoScroll = true;
		bool m_collapse = false;
		bool m_showTime = true;
		std::unordered_map<std::string, bool> m_categoryHidden;
	};
} // namespace aether::editor
