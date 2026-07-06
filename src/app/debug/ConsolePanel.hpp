#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	// In-editor log console over the engine's LogRingBuffer: clickable level count
	// badges, per-category filtering, a fuzzy text filter, consecutive-collapse,
	// timestamps, click-to-source, copy-to-clipboard and save-to-file. Starts
	// hidden (niche panel).
	class ConsolePanel final : public DebugPanel
	{
	public:
		[[nodiscard]] std::string_view GetName() const override
		{
			return "Console";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(LayerContext& context) override;

	private:
		char m_filter[128] = {};
		bool m_showVerbose = false;
		bool m_showInfo = true;
		bool m_showWarn = true;
		bool m_showError = true;
		bool m_autoScroll = true;
		bool m_collapse = false;
		bool m_showTime = true;
		// Categories the user has explicitly hidden (absent = shown). Discovered
		// from the live log, so new categories appear enabled by default.
		std::unordered_map<std::string, bool> m_categoryHidden;
	};
} // namespace aether::app
