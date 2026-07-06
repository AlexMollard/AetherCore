#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	class PostProcessingPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Post Processing";
		}

		void OnImGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;
	};
} // namespace aether::app
