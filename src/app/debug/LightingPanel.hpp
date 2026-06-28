#pragma once

#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	class LightingPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Lighting";
		}

		void OnUpdate(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

	private:
		bool m_lightGizmos = true;
		bool m_lightGizmoPointVolumes = true;
		bool m_lightGizmoSpotCones = true;
		bool m_lightGizmoSunDirection = true;
		bool m_lightGizmoShadowMarkers = true;
		float m_lightGizmoScale = 1.0f;
	};
} // namespace aether::app
