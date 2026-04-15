#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "AppLayer.hpp"
#include "Renderer.hpp"

namespace aether::app
{
	class DebugLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		static constexpr std::size_t kFrameHistorySize = 64;

		static const char* GetTonemapModeName(aether::TonemapMode mode);
		float GetAverageFrameTimeMs() const;
		void DrawDebugLine(aether::UIRenderer& ui, std::string_view text, float y) const;
		void DrawFrameTimeGraph(aether::UIRenderer& ui, float x, float y, float width, float height) const;

		std::array<float, kFrameHistorySize> m_frameTimesMs{};
		std::size_t m_frameHistoryHead = 0;
		std::size_t m_frameHistoryCount = 0;
	};
}
