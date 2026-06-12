#include "passes/PostProcessStack.hpp"

#include <cstring>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "rendering/RenderGraph.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	PostProcessStack PostProcessStack::Create(const Desc& desc)
	{
		PostProcessStack stack;

		AE_EXPECT_OR_THROW(hdrImage,
		        UniqueImage::Create(desc.device,
		                desc.allocator,
		                {
		                        .extent = desc.extent,
		                        .format = gpu::Format::R16G16B16A16Sfloat,
		                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                        .debugName = "PostProcess.HdrColor",
		                }));
		stack.m_hdrColorImage = std::move(hdrImage);
		stack.m_hdrColor = desc.renderGraph->RegisterImage(static_cast<void*>(stack.m_hdrColorImage.Get()), static_cast<void*>(stack.m_hdrColorImage.GetDefaultView()));
		AE_EXPECT_OR_THROW_VOID(stack.m_hdrColorImage.EnsureBindlessSampled(*desc.bindlessManager, desc.device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

		AE_EXPECT_OR_THROW(ldrImage,
		        UniqueImage::Create(desc.device,
		                desc.allocator,
		                {
		                        .extent = desc.extent,
		                        .format = gpu::Format::R8G8B8A8Unorm,
		                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                        .debugName = "PostProcess.LdrColor",
		                }));
		stack.m_ldrColorImage = std::move(ldrImage);
		stack.m_ldrColor = desc.renderGraph->RegisterImage(static_cast<void*>(stack.m_ldrColorImage.Get()), static_cast<void*>(stack.m_ldrColorImage.GetDefaultView()));
		AE_EXPECT_OR_THROW_VOID(stack.m_ldrColorImage.EnsureBindlessSampled(*desc.bindlessManager, desc.device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

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
		if (m_ldrColorImage.GetBindlessSampledSlot() != 0)
		{
			m_ldrColorImage.ReleaseBindlessSampled(true);
		}
		m_ldrColorImage.Reset();
		m_ldrColor = RGImage{};
		if (m_hdrColorImage.GetBindlessSampledSlot() != 0)
		{
			m_hdrColorImage.ReleaseBindlessSampled(true);
		}
		m_hdrColorImage.Reset();
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
			                push.hdrSlot = m_hdrColorImage.GetBindlessSampledSlot();
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
			                gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());

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
			                push.ldrSlot = m_ldrColorImage.GetBindlessSampledSlot();
			                push.fxaaEnabled = m_fxaaEnabled ? 1u : 0u;
			                cmd.PushConstantsRaw(m_fxaaPipeline.GetLayout(), gpu::ShaderStage::Fragment, 0, gpu::AsPushConstantBytes(push));

			                cmd.Draw(3, 1, 0, 0);
		                });
	}
} // namespace aether
