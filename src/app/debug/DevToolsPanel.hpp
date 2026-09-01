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
			return "Dev Tools";
		}


		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		// What the USER asked for, kept apart from the engine flag the renderer reads. The
		// two differ while the game is playing, and persisting the engine flag instead would
		// quietly save "off" every time the editor happened to be in play mode.
		bool m_physicsShapesWanted = false;
	};
} // namespace aether::editor
