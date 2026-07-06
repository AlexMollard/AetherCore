#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	// In-editor log console over the engine's LogRingBuffer: per-level toggles,
	// a fuzzy text filter, autoscroll and clear. Starts hidden (niche panel).
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
	};
} // namespace aether::app
