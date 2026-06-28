#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "debug/DebugPanel.hpp"

namespace aether::app
{
	class PerformancePanel final : public DebugPanel
	{
	public:
		static constexpr std::size_t kFrameSampleCount = 180;

		std::string_view GetName() const override
		{
			return "Performance";
		}

		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;
		void LoadSettings(TomlConfig& config, LayerContext& context) override;
		void SaveSettings(TomlConfig& config, LayerContext& context) const override;

	private:
		void PushFrameSample(float frameMs);

		std::array<float, kFrameSampleCount> m_frameSamples{};
		std::array<float, kFrameSampleCount> m_orderedSamples{};
		std::array<float, kFrameSampleCount> m_sorted{};
		std::size_t m_frameSampleHead = 0;
		std::size_t m_frameSampleCount = 0;

		static constexpr float kTitleUpdateInterval = 0.5f;
		float m_titleFps = 0.0f;
		float m_titleMs = 0.0f;
		float m_titleAccum = 0.0f;
	};
} // namespace aether::app
