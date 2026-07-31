#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "debug/DebugPanel.hpp"
#include "utils/FrameStats.hpp"   // FrameStats / Smoothness appear in the signatures below
#include "utils/FrameTimeline.hpp"

namespace aether::editor
{
	// A view over the engine's FrameTimeline. Holds no history of its own: the ring is the
	// single source of truth, which is what removed the three parallel sample arrays this
	// panel used to carry.
	class PerformancePanel final : public DebugPanel
	{
	public:
		static constexpr std::size_t kDisplayFrames = 240;

		std::string_view GetName() const override
		{
			return "Performance";
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		void DrawVerdict(const FrameStats& stats) const;
		void DrawPacingStrip() const;
		void DrawPhaseBreakdown() const;
		void DrawSimVsReal() const;
		void DrawStutterList(const FrameStats& stats) const;

		// Reused every frame so drawing allocates nothing.
		std::vector<FrameTiming> m_frames;

		static constexpr float kTitleUpdateInterval = 0.5f;
		float m_titleFps = 0.0f;
		float m_titleMs = 0.0f;
		float m_titleAccum = 0.0f;
	};
} // namespace aether::editor
