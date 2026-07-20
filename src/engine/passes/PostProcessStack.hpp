#pragma once

#include <array>
#include <cstdint>
#include <span>

#include <glm/glm.hpp>

#include "gpu/BindlessManager.hpp"
#include "gpu/GpuTypes.hpp"
#include "passes/TonemapDefs.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "rendering/RenderGraph.hpp"

namespace aether
{

	// Lifetime contract
	class PostProcessStack
	{
	public:
		struct Desc
		{
			gpu::Device device = nullptr;
			gpu::Extent2D extent;
			gpu::Format swapchainFormat = gpu::Format::Undefined;
			BindlessManager* bindlessManager = nullptr;
			RenderGraph* renderGraph = nullptr;
		};

		PostProcessStack() = default;
		~PostProcessStack() = default;

		PostProcessStack(const PostProcessStack&) = delete;
		PostProcessStack& operator=(const PostProcessStack&) = delete;

		PostProcessStack(PostProcessStack&&) noexcept = default;
		PostProcessStack& operator=(PostProcessStack&&) noexcept = default;

		static PostProcessStack Create(const Desc& desc);
		void Destroy();

		// Format the engine's forward pass must use when writing the HDR buffer.
		[[nodiscard]] static constexpr gpu::Format GetForwardColorFormat()
		{
			return gpu::Format::R16G16B16A16Sfloat;
		}

		[[nodiscard]] RGImage GetHdrColor() const
		{
			return m_hdrColor;
		}

		[[nodiscard]] std::uint32_t GetHdrBindlessSlot() const
		{
			return m_hdrBindlessSlot;
		}

		[[nodiscard]] RGImage GetFinalColor() const
		{
			return m_finalColor;
		}

		[[nodiscard]] gpu::ImageView GetFinalColorImageView() const
		{
			return m_finalColorView;
		}

		[[nodiscard]] gpu::Extent2D GetExtent() const
		{
			return m_extent;
		}

		void SetOutputToTexture(bool enabled)
		{
			m_outputToTexture = enabled;
		}

		[[nodiscard]] bool IsOutputToTextureEnabled() const
		{
			return m_outputToTexture;
		}

		void SetTonemapMode(TonemapMode mode)
		{
			m_tonemapMode = mode;
		}

		[[nodiscard]] TonemapMode GetTonemapMode() const
		{
			return m_tonemapMode;
		}

		void SetExposure(float exposure)
		{
			m_exposure = exposure;
		}

		[[nodiscard]] float GetExposure() const
		{
			return m_exposure;
		}

		// Camera-owned background composited WYSIWYG in the tonemap pass.
		// mode mirrors CameraBackground (0 solid, 1 gradient, 2 sky => disabled).
		// stops pack xyz = display-space colour, w = position (0..1).
		static constexpr std::uint32_t kMaxBackgroundStops = 8;
		void SetBackgroundParams(std::uint32_t mode, float angleRadians, std::uint32_t stopCount, const std::array<glm::vec4, kMaxBackgroundStops>& stops)
		{
			m_bgMode = mode;
			m_bgAngleRadians = angleRadians;
			m_bgStopCount = stopCount;
			m_bgStops = stops;
		}

		[[nodiscard]] gpu::PipelineView GetTonemapPipeline() const
		{
			return m_tonemapPipeline.GetPipeline();
		}

		void SetFxaaEnabled(bool enabled)
		{
			m_fxaaEnabled = enabled;
		}

		[[nodiscard]] bool IsFxaaEnabled() const
		{
			return m_fxaaEnabled;
		}

		void SetDebugCompare(bool enabled)
		{
			m_debugCompare = enabled;
		}

		[[nodiscard]] bool IsDebugCompareEnabled() const
		{
			return m_debugCompare;
		}

		void SetDebugModeCount(std::uint32_t count)
		{
			m_debugModeCount = count;
		}

		[[nodiscard]] std::uint32_t GetDebugModeCount() const
		{
			return m_debugModeCount;
		}

		void SetHistogramCaptureEnabled(bool enabled)
		{
			if (m_histogramCaptureEnabled == enabled)
			{
				return;
			}

			m_histogramCaptureEnabled = enabled;
			if (!enabled)
			{
				for (auto& ready: m_perFrameHistogramReady)
				{
					ready = false;
				}
				m_histogramDataValid = false;
			}
		}

		[[nodiscard]] bool IsHistogramCaptureEnabled() const
		{
			return m_histogramCaptureEnabled;
		}

		void SetHistogramUpdatePeriod(std::uint32_t frames)
		{
			m_histogramUpdatePeriod = frames > 0u ? frames : 1u;
		}

		[[nodiscard]] std::uint32_t GetHistogramUpdatePeriod() const
		{
			return m_histogramUpdatePeriod;
		}

		void SetHistogramSampleStride(std::uint32_t stride)
		{
			m_histogramSampleStride = stride > 0u ? stride : 1u;
		}

		[[nodiscard]] std::uint32_t GetHistogramSampleStride() const
		{
			return m_histogramSampleStride;
		}

		[[nodiscard]] const float* GetHdrHistogramBins() const
		{
			return m_histogramBins;
		}

		[[nodiscard]] const float* GetLdrHistogramBins() const
		{
			return m_ldrHistogramBins;
		}

		[[nodiscard]] bool IsHistogramValid() const
		{
			return m_histogramDataValid;
		}

		static constexpr std::uint32_t GetHistogramBinCount()
		{
			return kHistogramBins;
		}

		static constexpr float GetHistogramLogMin()
		{
			return kHistogramLogMin;
		}

		static constexpr float GetHistogramLogMax()
		{
			return kHistogramLogMax;
		}

		// bindless must outlive the graph (it is captured by the pass lambdas).
		void RegisterPasses(RenderGraph& graph, BindlessManager& bindless);

		void UpdateBufferHandles(RenderGraph& graph, std::uint32_t frameSlot);

	private:
		gpu::TextureHandle m_hdrColorHandle;
		RGImage m_hdrColor{};
		std::uint32_t m_hdrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_tonemapPipeline;

		gpu::TextureHandle m_ldrColorHandle;
		RGImage m_ldrColor{};
		std::uint32_t m_ldrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_fxaaPipeline;
		gpu::TextureHandle m_finalColorHandle;
		RGImage m_finalColor{};
		gpu::ImageView m_finalColorView = nullptr;
		gpu::Format m_swapchainFormat = gpu::Format::Undefined;
		gpu::Extent2D m_extent;

		TonemapMode m_tonemapMode = TonemapMode::Reinhard;
		float m_exposure = 1.0f;
		bool m_fxaaEnabled = false;
		bool m_outputToTexture = false;
		bool m_debugCompare = false;
		std::uint32_t m_debugModeCount = 0;

		void ReadbackHistogram(std::uint32_t frameSlot);
		[[nodiscard]] bool ShouldRecordHistogram(std::uint32_t frameIndex) const;
		static constexpr std::uint32_t kHistogramBins = 256;
		static constexpr float kHistogramLogMin = -10.0f;
		static constexpr float kHistogramLogMax = 10.0f;

		std::uint32_t m_bgMode = 2; // 2 = SkyGradient (composite disabled)
		float m_bgAngleRadians = 0.0f;
		std::uint32_t m_bgStopCount = 0;
		std::array<glm::vec4, kMaxBackgroundStops> m_bgStops{};
		std::array<gpu::BufferHandle, kMaxFramesInFlight> m_backgroundBuffer{};

		gpu::PipelineHandle m_histogramPipeline;
		std::array<gpu::BufferHandle, kMaxFramesInFlight> m_histogramOutput{};
		RGBuffer m_histogramOutputRG{};
		bool m_perFrameHistogramReady[kMaxFramesInFlight]{};
		float m_histogramBins[kHistogramBins]{};
		float m_ldrHistogramBins[kHistogramBins]{};
		bool m_histogramDataValid = false;
		bool m_histogramCaptureEnabled = false;
		std::uint32_t m_histogramUpdatePeriod = 6;
		std::uint32_t m_histogramSampleStride = 4;
	};
} // namespace aether
