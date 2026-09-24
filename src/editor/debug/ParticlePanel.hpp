#pragma once

#include <string>
#include <string_view>

#include "debug/DebugPanel.hpp"
#include "particles/ParticleComponents.hpp"

namespace aether::editor
{
	// Authoring hub for particle emitters: a live in-panel preview (its own CPU
	// sim - no world entity), interactive controls (direction dial, range
	// sliders, colour ramp), presets, then commit to a new entity or the
	// selection. Nothing here touches the scene until you press a commit button.
	class ParticlePanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Particles";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void SyncPreview();

		ParticleEmitterComponent m_config;  // the authored config
		ParticleEmitterComponent m_preview; // running preview sim (owns its particles)
		bool m_playing = true;
		bool m_darkBg = true;
		std::string m_status;
		bool m_statusError = false;
	};
} // namespace aether::editor
