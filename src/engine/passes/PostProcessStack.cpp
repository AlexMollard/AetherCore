#include "passes/PostProcessStack.hpp"

#include <cstring>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	PostProcessStack PostProcessStack::Create(const Desc& desc)
	{
		(void) desc.allocator;
		(void) desc.device;
		PostProcessStack stack;

		const gpu::TextureDesc hdrDesc{
		        .format = gpu::Format::R16G16B16A16Sfloat,
		        .extent = desc.extent,
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "PostProcess.HdrColor",
		};
		stack.m_hdrColorHandle = gpu::ResourceRegistry::CreateTexture(hdrDesc);
		if (!stack.m_hdrColorHandle.IsValid())
		{
			Throw(AetherError::Engine("PostProcessStack: HdrColor CreateTexture failed"));
		}
		stack.m_hdrColor = desc.renderGraph->RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(stack.m_hdrColorHandle), gpu::ResourceRegistry::ResolveTexture(stack.m_hdrColorHandle).view);
		{
			const auto slot = desc.bindlessManager->AllocateSampledImageSlot();
			if (!slot)
			{
				Throw(AetherError::Engine("PostProcessStack: HdrColor AllocateSampledImageSlot failed"));
			}
			stack.m_hdrBindlessSlot = *slot;
			const auto sampler = desc.bindlessManager->GetOrCreateSampler(gpu::Filter::Linear, gpu::SamplerMipmapMode::Linear, gpu::SamplerAddressMode::ClampToEdge);
			if (!sampler)
			{
				Throw(AetherError::Engine("PostProcessStack: HdrColor GetOrCreateSampler failed"));
			}
			// NOLINT(bugprone-unused-return-value): UpdateSampledImage only fails on
			// invalid slot/image/sampler; the slot was just allocated and the sampler
			// was just created above, so a failure indicates a logic bug, not a
			// recoverable error here. Assert in debug, swallow in release.
			(void) desc.bindlessManager->UpdateSampledImage(stack.m_hdrBindlessSlot, gpu::ResourceRegistry::ResolveTexture(stack.m_hdrColorHandle).view, *sampler, gpu::ImageLayout::ShaderReadOnly);
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
		{
			const auto slot = desc.bindlessManager->AllocateSampledImageSlot();
			if (!slot)
			{
				Throw(AetherError::Engine("PostProcessStack: LdrColor AllocateSampledImageSlot failed"));
			}
			stack.m_ldrBindlessSlot = *slot;
			const auto sampler = desc.bindlessManager->GetOrCreateSampler(gpu::Filter::Linear, gpu::SamplerMipmapMode::Linear, gpu::SamplerAddressMode::ClampToEdge);
			if (!sampler)
			{
				Throw(AetherError::Engine("PostProcessStack: LdrColor GetOrCreateSampler failed"));
			}
			// NOLINT(bugprone-unused-return-value): see m_hdr branch above.
			(void) desc.bindlessManager->UpdateSampledImage(stack.m_ldrBindlessSlot, gpu::ResourceRegistry::ResolveTexture(stack.m_ldrColorHandle).view, *sampler, gpu::ImageLayout::ShaderReadOnly);
		}

		const aether::gpu::DescriptorSetLayout bindlessLayout = desc.bindlessManager->GetLayout();

		AE_EXPECT_OR_THROW(tonemapPipeline,
		        GraphicsPipeline::Create(desc.device,
		                desc.pipelineCache,
		                {
		                        .shaderVfsPath = "shaders://tonemap.spv",
		                        .colorFormat = gpu::Format::R8G8B8A8Unorm,
		                        .pushConstantSize = 3 * sizeof(uint32_t),
		                        .pushConstantStages = gpu::ShaderStage::Fragment,
		                        .setLayouts = std::span<const aether::gpu::DescriptorSetLayout>(&bindlessLayout, 1),
		                }));
		stack.m_tonemapPipeline = std::move(tonemapPipeline);

		AE_EXPECT_OR_THROW(fxaaPipeline,
		        GraphicsPipeline::Create(desc.device,
		                desc.pipelineCache,
		                {
		                        .shaderVfsPath = "shaders://fxaa.spv",
		                        .colorFormat = desc.swapchainFormat,
		                        .pushConstantSize = 2u * sizeof(uint32_t),
		                        .pushConstantStages = gpu::ShaderStage::Fragment,
		                        .setLayouts = std::span<const aether::gpu::DescriptorSetLayout>(&bindlessLayout, 1),
		                }));
		stack.m_fxaaPipeline = std::move(fxaaPipeline);

		stack.m_swapchainFormat = desc.swapchainFormat;

		return stack;
	}

	void PostProcessStack::Destroy()
	{
		m_fxaaPipeline.Destroy();
		m_tonemapPipeline.Destroy();
		if (m_ldrBindlessSlot != 0xFFFFFFFFu)
		{
		}
		if (m_ldrColorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_ldrColorHandle);
		}
		m_ldrColorHandle = {};
		m_ldrColor = RGImage{};
		m_ldrBindlessSlot = 0xFFFFFFFFu;
		if (m_hdrBindlessSlot != 0xFFFFFFFFu)
		{
		}
		if (m_hdrColorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_hdrColorHandle);
		}
		m_hdrColorHandle = {};
		m_hdrColor = RGImage{};
		m_hdrBindlessSlot = 0xFFFFFFFFu;
		m_hdrColor = RGImage{};
	}

	void PostProcessStack::RegisterPasses(RenderGraph& graph, BindlessManager& bindless)
	{
		// Tonemap -> LDR intermediate (always), then FXAA -> swapchain.
		// FXAA toggling is handled at runtime via a push constant so the graph
		// topology stays stable and toggles don't require a graph rebuild.
		graph.AddPass("$PostProcess")
		        .ReadTexture(m_hdrColor)
		        .WriteColor(m_ldrColor, gpu::LoadOp::DontCare, gpu::StoreOp::Store, {})
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
			                (void) bindless;

			                cmd.BindPipeline(m_tonemapPipeline.GetPipeline(), m_tonemapPipeline.GetLayout());
			                cmd.BindDescriptorSet(m_tonemapPipeline.GetLayout(), 0, bindless.GetSet());

			                struct
			                {
				                std::uint32_t hdrSlot;
				                std::uint32_t mode;
				                float exposure;
			                } push;
			                push.hdrSlot = m_hdrBindlessSlot;
			                push.mode = static_cast<std::uint32_t>(m_tonemapMode);
			                push.exposure = m_exposure;
			                cmd.PushConstantsRaw(m_tonemapPipeline.GetLayout(), gpu::ShaderStage::Fragment, 0, gpu::AsPushConstantBytes(push));

			                cmd.Draw(3, 1, 0, 0);
		                });

		graph.AddPass("$FXAA")
		        .ReadTexture(m_ldrColor)
		        .WriteColor(graph.GetSwapchainColor(), gpu::LoadOp::DontCare, gpu::StoreOp::Store, {})
		        .Execute(
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

			                cmd.BindPipeline(m_fxaaPipeline.GetPipeline(), m_fxaaPipeline.GetLayout());
			                cmd.BindDescriptorSet(m_fxaaPipeline.GetLayout(), 0, bindless.GetSet());

			                struct
			                {
				                std::uint32_t ldrSlot;
				                std::uint32_t fxaaEnabled;
			                } push;
			                push.ldrSlot = m_ldrBindlessSlot;
			                push.fxaaEnabled = m_fxaaEnabled ? 1u : 0u;
			                cmd.PushConstantsRaw(m_fxaaPipeline.GetLayout(), gpu::ShaderStage::Fragment, 0, gpu::AsPushConstantBytes(push));

			                cmd.Draw(3, 1, 0, 0);
		                });
	}
} // namespace aether
