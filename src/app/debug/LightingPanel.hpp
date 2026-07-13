#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	class LightingPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Lighting";
		}

		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		bool m_lightGizmos = true;
		bool m_lightGizmoPointVolumes = true;
		bool m_lightGizmoSpotCones = true;
		bool m_lightGizmoSunDirection = true;
		bool m_lightGizmoShadowMarkers = true;
		float m_lightGizmoScale = 1.0f;
	};
} // namespace aether::editor
