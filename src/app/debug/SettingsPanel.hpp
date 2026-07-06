#pragma once

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	// Auto-generated engine settings editor. Every widget is produced from
	// aether::ForEachSettingField, so a setting added to EngineSettings shows up
	// here automatically with no per-field UI code. Edits go to the registered
	// EngineSettings service and persist via the app's shutdown save; settings
	// owned by a live subsystem (currently FXAA) are also applied immediately.
	class SettingsPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Settings";
		}

		void OnImGui(LayerContext& context) override;
	};
} // namespace aether::app
