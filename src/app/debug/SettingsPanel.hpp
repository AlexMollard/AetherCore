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
	};
} // namespace aether::editor
