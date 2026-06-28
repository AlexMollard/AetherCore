#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	class DevToolsPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "DevTools";
		}

		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

	private:
		bool m_debugTestShapes = true;
	};
} // namespace aether::app
