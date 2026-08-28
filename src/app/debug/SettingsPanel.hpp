#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class SettingsPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Settings";
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		// Substring match on the fully-qualified key AND the description, so "latency" finds
		// it across sections and "greyscale" finds saturation.
		char m_filter[64] = {};
		bool m_onlyModified = false;
	};
} // namespace aether::editor
