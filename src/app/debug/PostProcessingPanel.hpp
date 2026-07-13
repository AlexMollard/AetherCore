#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class PostProcessingPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Post Processing";
		}

		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;
	};
} // namespace aether::editor
