#include "passes/PostProcessStack.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include <glm/glm.hpp>
#include <glm/gtc/packing.hpp>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	PostProcessStack PostProcessStack::Create(const Desc& desc)
	{
		AE_PROFILE_ZONE();
		PostProcessStack stack;

		// HdrColor and LdrColor are single-frame: the forward pass writes HDR and $PostProcess
		// reads it, $PostProcess writes LDR and $FXAA reads it. Nothing outside the graph ever
		// looks at either, so the graph owns their memory. FinalColor below is the exception -
		// it carries an ImGui texture id and cannot move.
		constexpr gpu::ImageUsage kSampledSrc = gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc;

		stack.m_hdrColor = desc.renderGraph->CreateTransientColor(gpu::Format::R16G16B16A16Sfloat, desc.extent, kSampledSrc);
		stack.m_hdrBindlessSlot = desc.renderGraph->EnsureBindlessSampled(stack.m_hdrColor);
		if (stack.m_hdrBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("PostProcessStack: HdrColor bindless registration failed"));
		}

		stack.m_ldrColor = desc.renderGraph->CreateTransientColor(gpu::Format::R8G8B8A8Unorm, desc.extent, kSampledSrc);
		stack.m_ldrBindlessSlot = desc.renderGraph->EnsureBindlessSampled(stack.m_ldrColor);
		if (stack.m_ldrBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("PostProcessStack: LdrColor bindless registration failed"));
		}

		// Bloom mips, each half the previous. Transient like the HDR target: written and
		// consumed entirely inside one frame.
		gpu::Extent2D mipExtent = desc.extent;
		for (std::uint32_t i = 0; i < kBloomMipCount; ++i)
		{
			mipExtent.width = std::max(1u, mipExtent.width / 2u);
			mipExtent.height = std::max(1u, mipExtent.height / 2u);
			stack.m_bloomExtents[i] = mipExtent;
			stack.m_bloomMips[i] = desc.renderGraph->CreateTransientColor(gpu::Format::R16G16B16A16Sfloat, mipExtent, kSampledSrc);
			stack.m_bloomSlots[i] = desc.renderGraph->EnsureBindlessSampled(stack.m_bloomMips[i]);
			if (stack.m_bloomSlots[i] == 0xFFFFFFFFu)
			{
				Throw(AetherError::Engine("PostProcessStack: bloom mip bindless registration failed"));
			}
		}

		AE_EXPECT_OR_THROW(bloomDownPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://bloom.spv",
		                        .fragmentEntry = "downsampleMain",
		                        .colorFormat = gpu::Format::R16G16B16A16Sfloat,
		                        .debugName = "Bloom.Downsample",
		                        .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
		                }));
		stack.m_bloomDownsamplePipeline = std::move(bloomDownPipeline);

		// Additive: each upsample adds its blurred level onto the one above, which is
		// what turns the chain into a sum of radii rather than only the last one.
		AE_EXPECT_OR_THROW(bloomUpPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://bloom.spv",
		                        .fragmentEntry = "upsampleMain",
		                        .colorFormat = gpu::Format::R16G16B16A16Sfloat,
		                        .blendEnable = true,
		                        .blendMode = gpu::BlendMode::Additive,
		                        .debugName = "Bloom.Upsample",
		                        .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
		                }));
		stack.m_bloomUpsamplePipeline = std::move(bloomUpPipeline);

		const gpu::TextureDesc finalDesc{
		        .format = desc.swapchainFormat,
		        .extent = desc.extent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "PostProcess.FinalColor",
		};
		stack.m_finalColorHandle = gpu::ResourceRegistry::CreateTexture(finalDesc);
		if (!stack.m_finalColorHandle.IsValid())
		{
			Throw(AetherError::Engine("PostProcessStack: FinalColor CreateTexture failed"));
		}
		const auto& finalTexture = gpu::ResourceRegistry::ResolveTexture(stack.m_finalColorHandle);
		stack.m_finalColorView = finalTexture.view;
		stack.m_finalColor = desc.renderGraph->RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(stack.m_finalColorHandle), finalTexture.view);

		AE_EXPECT_OR_THROW(tonemapPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://tonemap.spv",
		                        .colorFormat = gpu::Format::R8G8B8A8Unorm,
		                        .debugName = "Tonemap",
		                        .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
		                }));
		stack.m_tonemapPipeline = std::move(tonemapPipeline);

		AE_EXPECT_OR_THROW(fxaaPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://fxaa.spv",
		                        .colorFormat = desc.swapchainFormat,
		                        .debugName = "FXAA",
		                        .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
		                }));
		stack.m_fxaaPipeline = std::move(fxaaPipeline);

		{
			stack.m_histogramPipeline = gpu::ResourceRegistry::CreateComputePipeline(desc.device,
			        gpu::ComputePipelineDesc{
			                .shaderVfsPath = "shaders://luminance_histogram.spv",
			                .debugName = "LuminanceHistogram",
			                .descriptorHeapMappings = desc.bindlessManager->GetDescriptorHeapMappings(),
			        });
			if (!stack.m_histogramPipeline.IsValid())
			{
				Throw(AetherError::Engine("PostProcessStack: LuminanceHistogram CreateComputePipeline failed"));
			}

			constexpr auto kHistogramBufferSize = static_cast<std::size_t>(kHistogramBins) * 2u * sizeof(std::uint32_t);
			for (auto& buf: stack.m_histogramOutput)
			{
				buf = gpu::ResourceRegistry::CreateMappedBuffer({
				        .size = kHistogramBufferSize,
				        .usage = gpu::BufferUsage::ShaderDeviceAddress | gpu::BufferUsage::TransferDst,
				        .memoryUsage = gpu::MappedMemoryUsage::GpuToCpu,
				        .debugName = "PostProcess.HistogramOutput",
				});
				if (!buf.IsValid())
				{
					Throw(AetherError::Engine("PostProcessStack: HistogramOutput CreateMappedBuffer failed"));
				}
			}
		}

		{
			// Per-frame background params buffer read by the tonemap pass (BDA):
			// uint mode, stopCount; float angleRadians; uint pad; float4 stops[8].
			constexpr auto kBackgroundBufferSize = sizeof(std::uint32_t) * 4u + sizeof(glm::vec4) * kMaxBackgroundStops;
			for (auto& buf: stack.m_backgroundBuffer)
			{
				buf = gpu::ResourceRegistry::CreateMappedBuffer({
				        .size = kBackgroundBufferSize,
				        .usage = gpu::BufferUsage::ShaderDeviceAddress,
				        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
				        .debugName = "PostProcess.BackgroundParams",
				});
				if (!buf.IsValid())
				{
					Throw(AetherError::Engine("PostProcessStack: BackgroundParams CreateMappedBuffer failed"));
				}
			}
		}

		stack.m_swapchainFormat = desc.swapchainFormat;
		stack.m_extent = desc.extent;

		return stack;
	}

	void PostProcessStack::Destroy()
	{
		AE_PROFILE_ZONE();
		m_fxaaPipeline.Destroy();
		m_tonemapPipeline.Destroy();
		// The HDR and LDR slots are the graph's to release: RenderGraph::Clear() drops every
		// slot right after this returns, and Shutdown() does the same on the teardown path.
		m_ldrColor = RGImage{};
		m_ldrBindlessSlot = 0xFFFFFFFFu;
		if (m_finalColorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_finalColorHandle);
		}
		m_finalColorHandle = {};
		m_finalColor = RGImage{};
		m_finalColorView = nullptr;
		m_hdrColor = RGImage{};
		m_hdrBindlessSlot = 0xFFFFFFFFu;
		for (auto& buf: m_backgroundBuffer)
		{
			if (buf.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf);
			}
			buf = {};
		}
		if (m_histogramPipeline.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_histogramPipeline);
		}
		m_histogramPipeline = {};
		for (auto& buf: m_histogramOutput)
		{
			if (buf.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf);
			}
			buf = {};
		}
		for (auto& ready: m_perFrameHistogramReady)
		{
			ready = false;
		}
		m_histogramOutputRG = {};
		m_histogramDataValid = false;
		m_extent = {};
		m_outputToTexture = false;
	}

	void PostProcessStack::ReadbackHistogram(const std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_histogramOutput[frameSlot % kMaxFramesInFlight]);
		if (!view.mappedPtr)
		{
			return;
		}

		constexpr gpu::DeviceSize kHistogramBufferSize = static_cast<gpu::DeviceSize>(kHistogramBins) * 2u * sizeof(std::uint32_t);
		gpu::ResourceRegistry::InvalidateMappedBuffer(m_histogramOutput[frameSlot % kMaxFramesInFlight], 0, kHistogramBufferSize);

		const std::uint32_t* bins = static_cast<const std::uint32_t*>(view.mappedPtr);

		auto normalise = [](const std::uint32_t* src, float* dst, std::uint32_t count)
		{
			float maxBin = 0.0f;
			for (std::uint32_t i = 0; i < count; ++i)
			{
				const float v = static_cast<float>(src[i]);
				if (v > maxBin)
				{
					maxBin = v;
				}
			}

			if (maxBin > 0.0f)
			{
				const float invMax = 1.0f / maxBin;
				for (std::uint32_t i = 0; i < count; ++i)
				{
					dst[i] = static_cast<float>(src[i]) * invMax;
				}
			}
			else
			{
				std::memset(dst, 0, count * sizeof(float));
			}
		};

		normalise(bins, m_histogramBins, kHistogramBins);
		normalise(bins + kHistogramBins, m_ldrHistogramBins, kHistogramBins);

		UpdateAutoExposure(bins);
	}

	// Drive exposure from the HDR histogram the probe graph was already building.
	//
	// The average is taken in log2 space, because luminance is perceived that way and
	// a linear mean lets one small bright region - a sun, a specular hit - drag the
	// whole frame dark. The darkest and brightest slices are discarded outright for
	// the same reason: a sky occupying a fifth of the screen should not decide the
	// exposure for the other four fifths.
	void PostProcessStack::UpdateAutoExposure(const std::uint32_t* bins)
	{
		if (!m_autoExposureEnabled)
		{
			return;
		}

		// Matches kLogMin / kLogRange in luminance_histogram.slang.
		constexpr float kLogMin = -10.0f;
		constexpr float kLogRange = 20.0f;
		constexpr float kLowPercentile = 0.30f;
		constexpr float kHighPercentile = 0.95f;

		double total = 0.0;
		// Bin 0 collects everything at or below the floor, which is mostly pixels with
		// no geometry at all. Counting it would peg the average to black.
		for (std::uint32_t i = 1; i < kHistogramBins; ++i)
		{
			total += bins[i];
		}
		if (total < 1.0)
		{
			return;
		}

		const double lowCount = total * kLowPercentile;
		const double highCount = total * kHighPercentile;
		double seen = 0.0;
		double weighted = 0.0;
		double weight = 0.0;
		for (std::uint32_t i = 1; i < kHistogramBins; ++i)
		{
			const double count = bins[i];
			if (count <= 0.0)
			{
				continue;
			}
			const double binStart = seen;
			seen += count;
			const double lo = std::max(binStart, lowCount);
			const double hi = std::min(seen, highCount);
			if (hi <= lo)
			{
				continue;
			}
			const float logLum = kLogMin + (static_cast<float>(i) / 255.0f) * kLogRange;
			weighted += logLum * (hi - lo);
			weight += (hi - lo);
		}
		if (weight <= 0.0)
		{
			return;
		}

		const float avgLuminance = std::exp2(static_cast<float>(weighted / weight));
		const float target = std::clamp(m_autoExposureKey / std::max(avgLuminance, 1e-4f), 0.03f, 30.0f);

		// Adapt in log space at a fixed rate, so going from bright to dark takes the
		// same time as the reverse. A linear approach would snap one way and crawl the
		// other, which reads as the image lurching.
		constexpr float kFrameDt = 1.0f / 60.0f;
		const float blend = 1.0f - std::exp(-m_autoExposureSpeed * kFrameDt);
		const float current = std::log2(std::max(m_autoExposureValue, 1e-4f));
		const float wanted = std::log2(target);
		m_autoExposureValue = std::exp2(current + (wanted - current) * blend);
	}

	void PostProcessStack::RegisterPasses(RenderGraph& graph, BindlessManager& bindless)
	{
		AE_PROFILE_ZONE();

		struct BloomPush
		{
			std::uint32_t srcSlot;
			float srcWidth;
			float srcHeight;
			float filterRadius;
			float karisAverage;
		};

		for (std::uint32_t i = 0; i < kBloomMipCount; ++i)
		{
			const RGImage src = (i == 0) ? m_hdrColor : m_bloomMips[i - 1];
			const std::uint32_t srcSlot = (i == 0) ? m_hdrBindlessSlot : m_bloomSlots[i - 1];
			const gpu::Extent2D srcExtent = (i == 0) ? m_extent : m_bloomExtents[i - 1];

			graph.AddFullscreenPass({
			                                .name = "$BloomDown" + std::to_string(i),
			                                .color = m_bloomMips[i],
			                                .extent = m_bloomExtents[i],
			                                .loadOp = gpu::LoadOp::DontCare,
			                                .consumes = (i == 0) ? std::vector<RenderGraph::FrameProductRef>{RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)} : std::vector<RenderGraph::FrameProductRef>{},
			                        })
			        .ReadTexture(src)
			        .Execute(
			                [this, &bindless, srcSlot, srcExtent, i](PassContext& ctx)
			                {
				                gpu::CommandList& cmd = ctx.recorder;
				                bindless.CmdBindGlobalResources(cmd);
				                cmd.BindPipeline(m_bloomDownsamplePipeline.GetPipeline());
				                const BloomPush push{
				                        .srcSlot = srcSlot,
				                        .srcWidth = static_cast<float>(srcExtent.width),
				                        .srcHeight = static_cast<float>(srcExtent.height),
				                        .filterRadius = 0.0f,
				                        // Firefly suppression only on the first step: past that the
				                        // signal is already averaged and re-weighting would dim it.
				                        .karisAverage = (i == 0) ? 1.0f : 0.0f,
				                };
				                cmd.PushDataRaw(0, std::as_bytes(std::span{&push, 1}));
				                cmd.Draw(3, 1, 0, 0);
			                });
		}

		for (std::uint32_t i = kBloomMipCount - 1; i > 0; --i)
		{
			const std::uint32_t srcIndex = i;
			const std::uint32_t dstIndex = i - 1;
			graph.AddFullscreenPass({
			                                .name = "$BloomUp" + std::to_string(dstIndex),
			                                .color = m_bloomMips[dstIndex],
			                                .extent = m_bloomExtents[dstIndex],
			                                .loadOp = gpu::LoadOp::Load,
			                        })
			        .ReadTexture(m_bloomMips[srcIndex])
			        .Execute(
			                [this, &bindless, srcIndex](PassContext& ctx)
			                {
				                gpu::CommandList& cmd = ctx.recorder;
				                bindless.CmdBindGlobalResources(cmd);
				                cmd.BindPipeline(m_bloomUpsamplePipeline.GetPipeline());
				                const BloomPush push{
				                        .srcSlot = m_bloomSlots[srcIndex],
				                        .srcWidth = static_cast<float>(m_bloomExtents[srcIndex].width),
				                        .srcHeight = static_cast<float>(m_bloomExtents[srcIndex].height),
				                        .filterRadius = m_bloomFilterRadius,
				                        .karisAverage = 0.0f,
				                };
				                cmd.PushDataRaw(0, std::as_bytes(std::span{&push, 1}));
				                cmd.Draw(3, 1, 0, 0);
			                });
		}
		// By VALUE. AddFullscreenPass hands back a prvalue and the chained calls return
		// references into it, so binding auto& here would leave a reference to an object
		// destroyed at the end of the statement - and a later ReadTexture would record
		// against whatever reused the storage, silently culling the producer it meant to
		// keep alive.
		auto postProcessPass = graph.AddFullscreenPass({
		        .name = "$PostProcess",
		        .color = m_ldrColor,
		        .extent = m_extent,
		        .loadOp = gpu::LoadOp::DontCare,
		        .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)},
		});
		postProcessPass.ReadTexture(m_hdrColor)
		        // Declared even though the slot reaches the shader through a push constant:
		        // the graph culls passes with no declared reader, and without this the last
		        // upsample vanished and mip 0 was read without a barrier.
		        .ReadTexture(m_bloomMips[0]);
		// Same reason for the out-of-focus image, declared whenever the target exists
		// rather than only when a lens is switched on - whether a camera has one is a
		// per-frame answer and the graph is built once.
		if (m_dofImage.IsValid())
		{
			postProcessPass.ReadTexture(m_dofImage);
		}
		postProcessPass.Execute(
		                [this, &bindless](PassContext& ctx)
		                {
			                gpu::CommandList& cmd = ctx.recorder;

			                bindless.CmdBindGlobalResources(cmd);

			                cmd.BindPipeline(m_tonemapPipeline.GetPipeline());

			                // Fill this frame's background params buffer (display-space
			                // WYSIWYG composite). mode 2 (sky) => addr 0 => tonemap
			                // passes through unchanged.
			                std::uint64_t backgroundParamsAddr = 0;
			                if (m_bgMode != 2u)
			                {
				                const std::uint32_t slot = ctx.frameSlot % kMaxFramesInFlight;
				                const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_backgroundBuffer[slot]);
				                if (view.mappedPtr != nullptr)
				                {
					                struct GpuBackgroundParams
					                {
						                std::uint32_t mode;
						                std::uint32_t stopCount;
						                float angleRadians;
						                std::uint32_t pad;
						                glm::vec4 stops[kMaxBackgroundStops];
					                } params{};
					                params.mode = m_bgMode;
					                params.stopCount = m_bgStopCount;
					                params.angleRadians = m_bgAngleRadians;
					                for (std::uint32_t i = 0; i < kMaxBackgroundStops; ++i)
					                {
						                params.stops[i] = m_bgStops[i];
					                }
					                std::memcpy(view.mappedPtr, &params, sizeof(params));
					                backgroundParamsAddr = gpu::ResourceRegistry::ResolveBuffer(m_backgroundBuffer[slot]).deviceAddress;
				                }
			                }

			                TonemapContracts::PushConstants push{};
			                push.hdrSlot = m_hdrBindlessSlot;
			                push.bloomSlot = (m_bloomStrength > 0.0f) ? m_bloomSlots[0] : 0xFFFFFFFFu;
			                push.bloomStrength = m_bloomStrength;
			                push.mode = static_cast<std::uint32_t>(m_tonemapMode);
			                // Manual exposure multiplies the adapted value rather than
				                // replacing it, so the slider stays a compensation control in
				                // stops rather than fighting the adaptation.
				                push.exposure = m_autoExposureEnabled ? (m_exposure * m_autoExposureValue) : m_exposure;
			                push.debugCompare = m_debugCompare ? 1u : 0u;
			                push.debugModeCount = m_debugModeCount;
			                push.inspectX = -1;
			                push.inspectY = -1;
			                push.screenWidth = m_extent.width;
			                push.screenHeight = m_extent.height;
			                push.dofSlot = m_dofSlot;
			                push.gradeContrast = m_gradeContrast;
			                push.gradeSaturation = m_gradeSaturation;
			                push.gradeTemperature = m_gradeTemperature;
			                push.gradeTint = m_gradeTint;
			                push.vignetteIntensity = m_vignetteIntensity;
			                push.vignetteRoundness = m_vignetteRoundness;
			                push.chromaticAberration = m_chromaticAberration;
			                push.filmGrain = m_filmGrain;
			                push.backgroundParamsAddr = backgroundParamsAddr;
			                push.motionBlurDepthSlot = m_sceneDepthSlot;
			                push.motionBlurStrength = m_motionBlurStrength;
			                push.motionBlurMaxRadiusPixels = m_motionBlurMaxRadiusPixels;
			                push.frameConstantsAddr = ctx.frameConstantsAddr;
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));

			                cmd.Draw(3, 1, 0, 0);
		                });

		auto fxaaPass = graph.AddPass("$FXAA");
		fxaaPass.ReadTexture(m_ldrColor).WriteColor(m_outputToTexture ? m_finalColor : aether::RenderGraph::GetSwapchainColor(), gpu::LoadOp::DontCare, gpu::StoreOp::Store, {});
		if (m_outputToTexture)
		{
			fxaaPass.SetExtent(m_extent);
		}
		fxaaPass.Execute(
		        [this, &bindless](PassContext& ctx)
		        {
			        gpu::CommandList cmd = ctx.recorder.View();

			        const gpu::Viewport vp{
			                .width = static_cast<float>(ctx.extent.width),
			                .height = static_cast<float>(ctx.extent.height),
			        };
			        const gpu::Rect2D scissor{
			                .width = ctx.extent.width,
			                .height = ctx.extent.height,
			        };
			        cmd.SetViewport(vp);
			        cmd.SetScissor(scissor);

			        bindless.CmdBindGlobalResources(cmd);

			        cmd.BindPipeline(m_fxaaPipeline.GetPipeline());

			        struct
			        {
				        std::uint32_t ldrSlot;
				        std::uint32_t fxaaEnabled;
				        float rcpWidth;
				        float rcpHeight;
				        float sharpness;
			        } push;
			        push.ldrSlot = m_ldrBindlessSlot;
			        push.fxaaEnabled = m_fxaaEnabled ? 1u : 0u;
			        // The edge search walks in UV across the LDR image, so it needs the size
			        // of THAT image - m_extent - not the size of whatever it is being drawn
			        // into. The two are the same only when the scene renders at the output
			        // resolution. Under a render scale they are not, and using the
			        // destination made every step of the search the wrong length: at 0.5 it
			        // walked half a texel at a time, so an edge it should have traced to its
			        // endpoint was abandoned partway and left half-antialiased.
			        push.rcpWidth = 1.0f / static_cast<float>(std::max(m_extent.width, 1u));
			        push.rcpHeight = 1.0f / static_cast<float>(std::max(m_extent.height, 1u));
			        push.sharpness = m_sharpness;
			        cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));

			        cmd.Draw(3, 1, 0, 0);
		        });

		m_histogramOutputRG = graph.RegisterBuffer(nullptr);

		graph.AddComputePass("$HistogramClear")
		        .DisableAsyncCompute()
		        .WriteBufferTransfer(m_histogramOutputRG)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                if (!ShouldRecordHistogram(ctx.frameIndex))
			                {
				                return;
			                }

			                const std::uint32_t slot = ctx.frameSlot % kMaxFramesInFlight;
			                constexpr gpu::DeviceSize kHistogramBufferSize = static_cast<gpu::DeviceSize>(kHistogramBins) * 2u * sizeof(std::uint32_t);
			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.FillBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_histogramOutput[slot]), 0, kHistogramBufferSize, 0);
		                });

		const auto histogramResolved = gpu::ResourceRegistry::ResolvePipeline(m_histogramPipeline);
		graph.AddComputePass("$Histogram")
		        .ReadTexture(m_hdrColor)
		        .ReadTexture(m_ldrColor)
		        .ReadWriteBuffer(m_histogramOutputRG)
		        .SetExtent(m_extent)
		        .ExecuteCompute(
		                [this, &bindless, histogramPipeline = histogramResolved.state](PassContext& ctx)
		                {
			                if (!ShouldRecordHistogram(ctx.frameIndex))
			                {
				                return;
			                }

			                // The grid is sized off a 1024x576 reference resolution, not the actual one, so
			                // every sample covers the same fraction of the screen whatever the render
			                // scale is. Metering a fixed slice of the world is the whole point: a
			                // grid that tracked the resolution would change what exposure measures
			                // every time the resolution moved.
			                constexpr std::uint32_t kMeterReferenceWidth = 1024u;
			                constexpr std::uint32_t kMeterReferenceHeight = 576u;
			                const std::uint32_t sampleStride = m_histogramSampleStride > 0u ? m_histogramSampleStride : 1u;
			                const std::uint32_t sampleWidth = std::max(kMeterReferenceWidth / sampleStride, 1u);
			                const std::uint32_t sampleHeight = std::max(kMeterReferenceHeight / sampleStride, 1u);
			                const std::uint32_t sampleCount = sampleWidth * sampleHeight;
			                if (sampleCount == 0u)
			                {
				                return;
			                }

			                const std::uint32_t slot = ctx.frameSlot % kMaxFramesInFlight;
			                const auto mappedView = gpu::ResourceRegistry::ResolveMappedBuffer(m_histogramOutput[slot]);

			                gpu::CommandList cmd = ctx.recorder.View();
			                bindless.CmdBindGlobalResources(cmd);
			                cmd.BindComputePipeline(histogramPipeline);

			                struct
			                {
				                std::uint32_t hdrSlot;
				                std::uint32_t ldrSlot;
				                std::uint64_t output;
				                std::uint32_t width;
				                std::uint32_t height;
				                std::uint32_t meterWidth;
				                std::uint32_t meterHeight;
			                } push;
			                push.hdrSlot = m_hdrBindlessSlot;
			                push.ldrSlot = m_ldrBindlessSlot;
			                push.output = mappedView.deviceAddress;
			                push.width = ctx.extent.width;
			                push.height = ctx.extent.height;
			                push.meterWidth = sampleWidth;
			                push.meterHeight = sampleHeight;
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));

			                constexpr std::uint32_t kHistogramThreads = 256;
			                cmd.Dispatch((sampleCount + kHistogramThreads - 1u) / kHistogramThreads, 1, 1);

			                m_perFrameHistogramReady[slot] = true;
		                });
	}

	void PostProcessStack::UpdateBufferHandles(RenderGraph& graph, const std::uint32_t frameSlot)
	{
		if (!m_histogramOutputRG.IsValid())
		{
			return;
		}

		const std::uint32_t slot = frameSlot % kMaxFramesInFlight;
		graph.UpdateExternalBuffer(m_histogramOutputRG, gpu::ResourceRegistry::ResolveBufferVkHandle(m_histogramOutput[slot]));

		if (NeedsHistogramReadback() && m_perFrameHistogramReady[slot])
		{
			ReadbackHistogram(slot);
			m_perFrameHistogramReady[slot] = false;
			m_histogramDataValid = true;
		}
	}

	bool PostProcessStack::ShouldRecordHistogram(const std::uint32_t frameIndex) const
	{
		// Auto-exposure needs this as much as the debug view does. Gated on the view alone,
		// the compute early-returned, the ready flag was never set, and exposure sat frozen at
		// its initial value for the whole run - in a shipped game, forever.
		if (!NeedsHistogramReadback())
		{
			return false;
		}

		const std::uint32_t updatePeriod = m_histogramUpdatePeriod > 0u ? m_histogramUpdatePeriod : 1u;
		return updatePeriod <= 1u || (frameIndex % updatePeriod) == 0u;
	}
} // namespace aether
