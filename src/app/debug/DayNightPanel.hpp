#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class DayNightPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Day / Night";
		}

		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		bool m_manualMode = false;
	};
} // namespace aether::editor
