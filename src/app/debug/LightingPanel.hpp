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
		// Off by default, unlike the other gizmos. A light's VOLUME is drawn at its true
		// radius, so a single default key light with a 12-unit range fills the viewport of a
		// brand-new project and hides the one cube in it. Every other editor shows a light's
		// range when you select the light, not permanently; until this panel can see the
		// selection, not-by-default is the closer approximation. The position marker and
		// shadow markers below still draw, so lights remain visible and clickable, and the
		// Lighting panel toggles volumes back on for anyone tuning falloff.
		bool m_lightGizmoPointVolumes = false;
		bool m_lightGizmoSpotCones = true;
		bool m_lightGizmoSunDirection = true;
		bool m_lightGizmoShadowMarkers = true;
		float m_lightGizmoScale = 1.0f;
	};
} // namespace aether::editor
