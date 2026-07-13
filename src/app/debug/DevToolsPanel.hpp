#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class DevToolsPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "DevTools";
		}

		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;
	};
} // namespace aether::editor
