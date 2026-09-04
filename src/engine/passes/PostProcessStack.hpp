#pragma once

#include <algorithm>
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

		// Which texture the tonemap samples the out-of-focus image from. Set per frame
		// because it follows the main camera, which a scene can change at any time.
		void SetDepthOfFieldSlot(std::uint32_t slot)
		{
			m_dofSlot = slot;
		}

		// Scene depth, which camera motion blur reprojects through. Unlike the DoF image
		// this needs no ReadTexture declaration: the depth buffer is produced and consumed
		// by passes the graph already orders, so nothing is culled by reading it here.
		void SetSceneDepthSlot(std::uint32_t slot)
		{
			m_sceneDepthSlot = slot;
		}

		// Shutter fraction of the frame interval - 0.5 is the film convention (a
		// 180-degree shutter) and 0 is off - and the ceiling in pixels that stops a
		// teleport smearing the whole screen for one frame.
		void SetMotionBlur(float strength, float maxRadiusPixels)
		{
			m_motionBlurStrength = strength;
			m_motionBlurMaxRadiusPixels = maxRadiusPixels;
		}

		[[nodiscard]] float GetMotionBlurStrength() const
		{
			return m_motionBlurStrength;
		}

		[[nodiscard]] float GetMotionBlurMaxRadiusPixels() const
		{
			return m_motionBlurMaxRadiusPixels;
		}

		// The image itself, not just its slot: the tonemap pass has to DECLARE the read or
		// the graph culls whatever produced it, since a slot travelling in a push constant
		// is invisible to the graph.
		void SetDepthOfFieldImage(RGImage image)
		{
			m_dofImage = image;
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

		// Colour grade, applied in LINEAR space before the tonemap curve. Identity by
		// default (contrast 1, saturation 1, no white-balance shift), so a project that
		// never touches these renders exactly as it did before.
		void SetGrade(float contrast, float saturation, float temperature, float tint)
		{
			m_gradeContrast = contrast;
			m_gradeSaturation = saturation;
			m_gradeTemperature = temperature;
			m_gradeTint = tint;
		}

		void SetVignette(float intensity, float roundness)
		{
			m_vignetteIntensity = intensity;
			m_vignetteRoundness = roundness;
		}

		void SetChromaticAberration(float pixels) { m_chromaticAberration = pixels; }

		void SetFilmGrain(float amount) { m_filmGrain = amount; }

		[[nodiscard]] float GetFilmGrain() const { return m_filmGrain; }

		void SetSharpness(float sharpness) { m_sharpness = sharpness; }

		[[nodiscard]] float GetSharpness() const { return m_sharpness; }

		[[nodiscard]] float GetChromaticAberration() const { return m_chromaticAberration; }

		[[nodiscard]] float GetVignetteIntensity() const { return m_vignetteIntensity; }
		[[nodiscard]] float GetVignetteRoundness() const { return m_vignetteRoundness; }

		[[nodiscard]] float GetGradeContrast() const { return m_gradeContrast; }
		[[nodiscard]] float GetGradeSaturation() const { return m_gradeSaturation; }
		[[nodiscard]] float GetGradeTemperature() const { return m_gradeTemperature; }
		[[nodiscard]] float GetGradeTint() const { return m_gradeTint; }

		void SetExposure(float exposure)
		{
			m_exposure = exposure;
		}

		void SetAutoExposureEnabled(bool enabled)
		{
			m_autoExposureEnabled = enabled;
		}

		[[nodiscard]] bool IsAutoExposureEnabled() const
		{
			return m_autoExposureEnabled;
		}

		void SetAutoExposureKey(float key)
		{
			m_autoExposureKey = std::clamp(key, 0.01f, 1.0f);
		}

		[[nodiscard]] float GetAutoExposureKey() const
		{
			return m_autoExposureKey;
		}

		void SetAutoExposureSpeed(float speed)
		{
			m_autoExposureSpeed = std::clamp(speed, 0.05f, 8.0f);
		}

		[[nodiscard]] float GetAutoExposureSpeed() const
		{
			return m_autoExposureSpeed;
		}

		// Exposure the adaptation has settled on, before the manual multiplier.
		[[nodiscard]] float GetAutoExposureValue() const
		{
			return m_autoExposureValue;
		}

		void SetBloomStrength(float strength)
		{
			m_bloomStrength = std::clamp(strength, 0.0f, 1.0f);
		}

		[[nodiscard]] float GetBloomStrength() const
		{
			return m_bloomStrength;
		}

		void SetBloomFilterRadius(float radius)
		{
			m_bloomFilterRadius = std::clamp(radius, 0.5f, 4.0f);
		}

		[[nodiscard]] float GetBloomFilterRadius() const
		{
			return m_bloomFilterRadius;
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
			m_bgStopCount = std::min(stopCount, kMaxBackgroundStops);
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

		// Coarseness of the metering grid: the grid is a 1024x576 reference divided by
		// this, so it is a fixed number of samples spread across the image rather than a
		// stride in real pixels. Exposure must not move when the render resolution does.
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
		// Graph-owned. The bindless slots are reserved against the graph, so they stay valid
		// while the images behind them move between frames and graph rebuilds.
		// Bloom chain. Each level is half the previous; the chain is walked down with a
		// 13-tap filter and back up with an additive 3x3 tent, so the widest level
		// contributes a broad glow and the narrowest a tight one - a single pass at one
		// radius cannot produce both.
		static constexpr std::uint32_t kBloomMipCount = 5;
		std::array<RGImage, kBloomMipCount> m_bloomMips{};
		std::array<std::uint32_t, kBloomMipCount> m_bloomSlots{};
		// 0xFFFFFFFF whenever the main camera is not using a lens, which is the usual case.
		std::uint32_t m_dofSlot = 0xFFFFFFFFu;
		std::uint32_t m_sceneDepthSlot = 0xFFFFFFFFu;
		float m_motionBlurStrength = 0.0f;
		float m_motionBlurMaxRadiusPixels = 64.0f;
		RGImage m_dofImage;
		std::array<gpu::Extent2D, kBloomMipCount> m_bloomExtents{};
		GraphicsPipeline m_bloomDownsamplePipeline;
		GraphicsPipeline m_bloomUpsamplePipeline;
		float m_bloomStrength = 0.05f;
		float m_bloomFilterRadius = 1.0f;

		RGImage m_hdrColor{};
		std::uint32_t m_hdrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_tonemapPipeline;

		RGImage m_ldrColor{};
		std::uint32_t m_ldrBindlessSlot = 0xFFFFFFFFu;
		GraphicsPipeline m_fxaaPipeline;
		// Persistent, unlike the two above: ViewportPanel registers this view as an ImGui
		// texture id, and an ImGui descriptor is created against one specific image.
		gpu::TextureHandle m_finalColorHandle;
		RGImage m_finalColor{};
		gpu::ImageView m_finalColorView = nullptr;
		gpu::Format m_swapchainFormat = gpu::Format::Undefined;
		gpu::Extent2D m_extent;

		// ACES by default. Reinhard has no shoulder worth the name - it compresses
		// everything toward white and takes the colour with it - which undercuts both
		// the bloom and the sky lighting feeding bright values into it.
		// Auto exposure. The histogram was already being computed every frame purely to
		// draw the HDR probe graph; this reads the same data and actually uses it.
		bool m_autoExposureEnabled = true;
		float m_autoExposureKey = 0.18f;   // middle grey the average is driven toward
		float m_autoExposureSpeed = 2.0f;  // adaptation rate, in e-folds per second
		float m_autoExposureValue = 1.0f;

		TonemapMode m_tonemapMode = TonemapMode::AcesFilmic;
		float m_exposure = 1.0f;
		float m_gradeContrast = 1.0f;
		float m_gradeSaturation = 1.0f;
		float m_gradeTemperature = 0.0f;
		float m_gradeTint = 0.0f;
		float m_vignetteIntensity = 0.0f;
		float m_vignetteRoundness = 1.0f;
		float m_chromaticAberration = 0.0f;
		float m_sharpness = 0.0f;
		float m_filmGrain = 0.0f;
		bool m_fxaaEnabled = true;
		bool m_outputToTexture = false;
		bool m_debugCompare = false;
		std::uint32_t m_debugModeCount = 0;

		void ReadbackHistogram(std::uint32_t frameSlot);
		void UpdateAutoExposure(const std::uint32_t* bins);
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

		// Whether the histogram has to be read back to the CPU this frame. The debug view is
		// one reason; auto-exposure is the other, and it is the one that ships.
		[[nodiscard]] bool NeedsHistogramReadback() const
		{
			return m_histogramCaptureEnabled || m_autoExposureEnabled;
		}
		std::uint32_t m_histogramUpdatePeriod = 6;
		std::uint32_t m_histogramSampleStride = 4;
	};
} // namespace aether
