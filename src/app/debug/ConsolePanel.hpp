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

		// Filter the Console down to the C# script errors and jump to the newest one.
		// Focusing the Console alone was not enough: with auto-scroll on it shows its newest
		// lines, so the failure sat far above the view and clicking the badge appeared to do
		// nothing at all.
		void ShowScriptErrors();
		// The same jump, but unfiltered: for engine problems, which have no shared prefix to
		// filter on and could be anything from a failed shader to a missing asset.
		void ShowLatestProblem();

	private:
		// Shared tail of both: make sure nothing hides the entry, then jump to it.
		void RevealProblem();

	public:

	private:
		char m_filter[128] = {};
		bool m_showVerbose = false;
		bool m_showInfo = true;
		bool m_showWarn = true;
		bool m_showError = true;
		bool m_autoScroll = true;
		// Set by RevealLatestProblem, consumed by the next draw, which is the only place that
		// knows where each row ended up on screen.
		bool m_revealProblem = false;
		bool m_collapse = false;
		bool m_showTime = true;
		std::unordered_map<std::string, bool> m_categoryHidden;
	};
} // namespace aether::editor
