#pragma once

#include <array>
#include <cstddef>

#include "AppLayer.hpp"
#include "rendering/Renderer.hpp"

namespace aether::app
{
	class DebugLayer final : public AppLayer
	{
	public:
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		static constexpr std::size_t kFrameHistorySize = 128;

		static const char* GetTonemapModeName(aether::TonemapMode mode);

		float GetAverageFrameTimeMs() const;
		float GetMinFrameTimeMs() const;
		float GetMaxFrameTimeMs() const;

		void DrawFrameTimeGraph() const;

		std::array<float, kFrameHistorySize> m_frameTimesMs{};
		std::size_t m_frameHistoryHead = 0;
		std::size_t m_frameHistoryCount = 0;
		bool m_visible = true;
	};
} // namespace aether::app
