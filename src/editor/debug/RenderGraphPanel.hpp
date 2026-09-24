#pragma once

#include <array>
#include <string>
#include <unordered_map>

#include "debug/DebugPanel.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether::editor
{
	class RenderGraphPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Render Graph";
		}

		void OnImGui(app::LayerContext& context) override;
		void LoadSettings(TomlConfig& config, app::LayerContext& context) override;
		void SaveSettings(TomlConfig& config, app::LayerContext& context) const override;

	private:
		static constexpr std::size_t kRenderBenchmarkSampleCount = 240;

		struct RenderPassBenchmark
		{
			std::array<float, kRenderBenchmarkSampleCount> samples{};
			std::size_t head = 0;
			std::size_t count = 0;
		};

		struct BenchmarkStats
		{
			float avg = 0.0f;
			float min = 0.0f;
			float max = 0.0f;
		};

		template<typename T>
		BenchmarkStats ComputeBenchmarkStats(const T& benchmark)
		{
			if (benchmark.count == 0)
			{
				return {};
			}

			float total = 0.0f;
			float minValue = benchmark.samples[0];
			float maxValue = benchmark.samples[0];
			for (std::size_t i = 0; i < benchmark.count; ++i)
			{
				const std::size_t idx = (benchmark.head + benchmark.samples.size() - benchmark.count + i) % benchmark.samples.size();
				const float value = benchmark.samples[idx];
				total += value;
				minValue = std::min(minValue, value);
				maxValue = std::max(maxValue, value);
			}
			return {.avg = total / static_cast<float>(benchmark.count), .min = minValue, .max = maxValue};
		}

		void DrawRenderGraphDebugger(app::LayerContext& context, RenderGraph& graph);

		std::unordered_map<std::string, RenderPassBenchmark> m_renderPassBenchmarks;
		std::string m_selectedRenderPass;
		char m_renderGraphFilter[96] = {};
		bool m_renderGraphShowDisabled = true;
		bool m_renderGraphShowCulled = true;
		bool m_renderGraphAutoSelectHotPass = false;
	};
} // namespace aether::editor
