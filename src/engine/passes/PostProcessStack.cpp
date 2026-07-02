#include "passes/PostProcessStack.hpp"

#include <algorithm>
#include <cmath>
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

		const gpu::TextureDesc hdrDesc{
		        .format = gpu::Format::R16G16B16A16Sfloat,
		        .extent = desc.extent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled | gpu::ImageUsage::TransferSrc,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "PostProcess.HdrColor",
		};
		stack.m_hdrColorHandle = gpu::ResourceRegistry::CreateTexture(hdrDesc);
		if (!stack.m_hdrColorHandle.IsValid())
		{
			Throw(AetherError::Engine("PostProcessStack: HdrColor CreateTexture failed"));
		}
		stack.m_hdrColor = desc.renderGraph->RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(stack.m_hdrColorHandle), gpu::ResourceRegistry::ResolveTexture(stack.m_hdrColorHandle).view);
		gpu::ResourceRegistry::EnsureBindlessSampled(stack.m_hdrColorHandle, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
		stack.m_hdrBindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(stack.m_hdrColorHandle);
		if (stack.m_hdrBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("PostProcessStack: HdrColor bindless registration failed"));
		}

		const gpu::TextureDesc ldrDesc{
		        .format = gpu::Format::R8G8B8A8Unorm,
		        .extent = desc.extent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "PostProcess.LdrColor",
		};
		stack.m_ldrColorHandle = gpu::ResourceRegistry::CreateTexture(ldrDesc);
		if (!stack.m_ldrColorHandle.IsValid())
		{
			Throw(AetherError::Engine("PostProcessStack: LdrColor CreateTexture failed"));
		}
		stack.m_ldrColor = desc.renderGraph->RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(stack.m_ldrColorHandle), gpu::ResourceRegistry::ResolveTexture(stack.m_ldrColorHandle).view);
		gpu::ResourceRegistry::EnsureBindlessSampled(stack.m_ldrColorHandle, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
		stack.m_ldrBindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(stack.m_ldrColorHandle);
		if (stack.m_ldrBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("PostProcessStack: LdrColor bindless registration failed"));
		}

		const gpu::TextureDesc finalDesc{
		        .format = desc.swapchainFormat,
		        .extent = desc.extent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
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

		// Compute pipeline + per-frame mapped output buffers for GPU luminance histogram
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

			constexpr auto kHistogramBufferSize = kHistogramBins * 2u * sizeof(std::uint32_t);
			for (auto& buf : stack.m_histogramOutput)
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

		stack.m_swapchainFormat = desc.swapchainFormat;
		stack.m_extent = desc.extent;

		return stack;
	}

	void PostProcessStack::Destroy()
	{
		AE_PROFILE_ZONE();
		m_fxaaPipeline.Destroy();
		m_tonemapPipeline.Destroy();
		if (m_ldrColorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_ldrColorHandle);
		}
		m_ldrColorHandle = {};
		m_ldrColor = RGImage{};
		m_ldrBindlessSlot = 0xFFFFFFFFu;
		if (m_finalColorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_finalColorHandle);
		}
		m_finalColorHandle = {};
		m_finalColor = RGImage{};
		m_finalColorView = nullptr;
		if (m_hdrColorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_hdrColorHandle);
		}
		m_hdrColorHandle = {};
		m_hdrColor = RGImage{};
		m_hdrBindlessSlot = 0xFFFFFFFFu;
		if (m_histogramPipeline.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_histogramPipeline);
		}
		m_histogramPipeline = {};
		for (auto& buf : m_histogramOutput)
		{
			if (buf.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf);
			}
			buf = {};
		}
		for (auto& ready : m_perFrameHistogramReady)
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

		constexpr gpu::DeviceSize kHistogramBufferSize = kHistogramBins * 2u * sizeof(std::uint32_t);
		gpu::ResourceRegistry::InvalidateMappedBuffer(m_histogramOutput[frameSlot % kMaxFramesInFlight], 0, kHistogramBufferSize);

		const std::uint32_t* bins = static_cast<const std::uint32_t*>(view.mappedPtr);

		auto normalise = [](const std::uint32_t* src, float* dst, std::uint32_t count)
		{
			float maxBin = 0.0f;
			for (std::uint32_t i = 0; i < count; ++i)
			{
				const float v = static_cast<float>(src[i]);
				if (v > maxBin) maxBin = v;
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

		normalise(bins,          m_histogramBins,     kHistogramBins); // HDR first 256
		normalise(bins + 256,    m_ldrHistogramBins,  kHistogramBins); // LDR next 256
	}

	void PostProcessStack::RegisterPasses(RenderGraph& graph, BindlessManager& bindless)
	{
		AE_PROFILE_ZONE();
		// Tonemap -> LDR intermediate (always), then FXAA -> swapchain.
		// FXAA toggling is handled at runtime via a push constant so the graph
		// topology stays stable and toggles don't require a graph rebuild.
		graph.AddFullscreenPass({
		                                .name = "$PostProcess",
		                                .color = m_ldrColor,
		                                .extent = m_extent,
		                                .loadOp = gpu::LoadOp::DontCare,
		                                .consumes = {RenderGraph::Product<FrameTextureProduct>(kFrameProductHdrColor)},
		                        })
		        .ReadTexture(m_hdrColor)
		        .Execute(
		                        [this, &bindless](PassContext& ctx)
		                        {
			                gpu::CommandList& cmd = ctx.recorder;

			                const gpu::Viewport vp{
			                        .width = static_cast<float>(ctx.extent.width),
			                        .height = static_cast<float>(ctx.extent.height),
			                };
			                const gpu::Rect2D scissor{
			                        .x = 0,
			                        .y = 0,
			                        .width = ctx.extent.width,
			                        .height = ctx.extent.height,
			                };
			                bindless.CmdBindHeaps(cmd);

			                cmd.BindPipeline(m_tonemapPipeline.GetPipeline());

			                struct
			                {
				                std::uint32_t hdrSlot;
				                std::uint32_t mode;
				                float exposure;
				                std::uint32_t debugCompare;
				                std::uint32_t debugModeCount;
				                std::int32_t inspectX;
				                std::int32_t inspectY;
				                std::uint32_t screenWidth;
			                std::uint32_t screenHeight;
			                } push;
			                push.hdrSlot = m_hdrBindlessSlot;
			                push.mode = static_cast<std::uint32_t>(m_tonemapMode);
			                push.exposure = m_exposure;
			                push.debugCompare = m_debugCompare ? 1u : 0u;
			                push.debugModeCount = m_debugModeCount;
			                push.inspectX = -1;
			                push.inspectY = -1;
			                push.screenWidth = m_extent.width;
			                push.screenHeight = m_extent.height;
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));

			                cmd.Draw(3, 1, 0, 0);
		                });

		auto fxaaPass = graph.AddPass("$FXAA");
		fxaaPass.ReadTexture(m_ldrColor).WriteColor(m_outputToTexture ? m_finalColor : graph.GetSwapchainColor(), gpu::LoadOp::DontCare, gpu::StoreOp::Store, {});
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

			        bindless.CmdBindHeaps(cmd);

			        cmd.BindPipeline(m_fxaaPipeline.GetPipeline());

			        struct
			        {
				        std::uint32_t ldrSlot;
				        std::uint32_t fxaaEnabled;
			        } push;
			        push.ldrSlot = m_ldrBindlessSlot;
			        push.fxaaEnabled = m_fxaaEnabled ? 1u : 0u;
			        cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));

			        cmd.Draw(3, 1, 0, 0);
		        });

		// GPU luminance histogram (debug). The per-frame-slot output buffer is
		// registered as an external graph buffer whose backing is swapped each
		// frame via UpdateBufferHandles; clear and dispatch are separate passes
		// so the graph owns all synchronization between them.
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
			                constexpr gpu::DeviceSize kHistogramBufferSize = kHistogramBins * 2u * sizeof(std::uint32_t);
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
		                [this, &bindless, histogramPipeline = const_cast<void*>(histogramResolved.state)](PassContext& ctx)
		                {
			                if (!ShouldRecordHistogram(ctx.frameIndex))
			                {
				                return;
			                }

			                const std::uint32_t sampleStride = m_histogramSampleStride > 0u ? m_histogramSampleStride : 1u;
			                const std::uint32_t sampleWidth = (ctx.extent.width + sampleStride - 1u) / sampleStride;
			                const std::uint32_t sampleHeight = (ctx.extent.height + sampleStride - 1u) / sampleStride;
			                const std::uint32_t sampleCount = sampleWidth * sampleHeight;
			                if (sampleCount == 0u)
			                {
				                return;
			                }

			                const std::uint32_t slot = ctx.frameSlot % kMaxFramesInFlight;
			                const auto mappedView = gpu::ResourceRegistry::ResolveMappedBuffer(m_histogramOutput[slot]);

			                gpu::CommandList cmd = ctx.recorder.View();
			                bindless.CmdBindHeaps(cmd);
			                cmd.BindComputePipeline(histogramPipeline);

			                struct
			                {
				                std::uint32_t hdrSlot;
				                std::uint32_t ldrSlot;
				                std::uint64_t output;
				                std::uint32_t width;
				                std::uint32_t height;
				                std::uint32_t sampleStride;
			                } push;
			                push.hdrSlot = m_hdrBindlessSlot;
			                push.ldrSlot = m_ldrBindlessSlot;
			                push.output = mappedView.deviceAddress;
			                push.width = ctx.extent.width;
			                push.height = ctx.extent.height;
			                push.sampleStride = sampleStride;
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

		// Readback this slot's output from kMaxFramesInFlight frames ago
		// (guaranteed complete by frame pacing).
		if (m_histogramCaptureEnabled && m_perFrameHistogramReady[slot])
		{
			ReadbackHistogram(slot);
			m_perFrameHistogramReady[slot] = false;
			m_histogramDataValid = true;
		}
	}

	bool PostProcessStack::ShouldRecordHistogram(const std::uint32_t frameIndex) const
	{
		if (!m_histogramCaptureEnabled)
		{
			return false;
		}

		const std::uint32_t updatePeriod = m_histogramUpdatePeriod > 0u ? m_histogramUpdatePeriod : 1u;
		return updatePeriod <= 1u || (frameIndex % updatePeriod) == 0u;
	}
} // namespace aether
