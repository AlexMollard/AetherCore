#include "passes/PostProcessStack.hpp"

#include "vulkan/volk.hpp"

#include "rendering/CommandRecorder.hpp"
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
		                        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
		                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                        .debugName = "PostProcess.HdrColor",
		                }));
		stack.m_hdrColorImage = std::move(hdrImage);
		stack.m_hdrColor = desc.renderGraph->RegisterImage(stack.m_hdrColorImage.Get(), stack.m_hdrColorImage.GetDefaultView());
		AE_EXPECT_OR_THROW_VOID(stack.m_hdrColorImage.EnsureBindlessSampled(*desc.bindlessManager, desc.device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

		AE_EXPECT_OR_THROW(ldrImage,
		        UniqueImage::Create(desc.device,
		                desc.allocator,
		                {
		                        .extent = desc.extent,
		                        .format = VK_FORMAT_R8G8B8A8_UNORM,
		                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                        .debugName = "PostProcess.LdrColor",
		                }));
		stack.m_ldrColorImage = std::move(ldrImage);
		stack.m_ldrColor = desc.renderGraph->RegisterImage(stack.m_ldrColorImage.Get(), stack.m_ldrColorImage.GetDefaultView());
		AE_EXPECT_OR_THROW_VOID(stack.m_ldrColorImage.EnsureBindlessSampled(*desc.bindlessManager, desc.device, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));

		const VkDescriptorSetLayout bindlessLayout = desc.bindlessManager->GetLayout();

		AE_EXPECT_OR_THROW(tonemapPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://tonemap.slang.spv",
		                        .colorFormat = VK_FORMAT_R8G8B8A8_UNORM,
		                        .pushConstantSize = 3 * sizeof(uint32_t),
		                        .pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT,
		                        .setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
		                }));
		stack.m_tonemapPipeline = std::move(tonemapPipeline);

		AE_EXPECT_OR_THROW(fxaaPipeline,
		        GraphicsPipeline::Create(desc.device,
		                {
		                        .shaderVfsPath = "shaders://fxaa.slang.spv",
		                        .colorFormat = desc.swapchainFormat,
		                        .pushConstantSize = 2u * sizeof(uint32_t),
		                        .pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT,
		                        .setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
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
		        .WriteColor(m_ldrColor, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {})
		        .Execute(
		                [this, &bindless](PassContext& ctx)
		                {
			                const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

			                const VkViewport vp{
				                .x = 0.0f,
				                .y = 0.0f,
				                .width = static_cast<float>(ctx.extent.width),
				                .height = static_cast<float>(ctx.extent.height),
				                .minDepth = 0.0f,
				                .maxDepth = 1.0f,
			                };
			                const VkRect2D scissor{
				                { 0, 0 },
                                ctx.extent
			                };
			                vkCmdSetViewport(cmd, 0, 1, &vp);
			                vkCmdSetScissor(cmd, 0, 1, &scissor);

			                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline.GetPipeline());

			                const VkDescriptorSet set = bindless.GetSet();
			                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline.GetLayout(), 0, 1, &set, 0, nullptr);

			                struct
			                {
				                uint32_t hdrSlot;
				                uint32_t mode;
				                float exposure;
			                } push;
			                push.hdrSlot = m_hdrColorImage.GetBindlessSampledSlot();
			                push.mode = static_cast<uint32_t>(m_tonemapMode);
			                push.exposure = m_exposure;
			                vkCmdPushConstants(cmd, m_tonemapPipeline.GetLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

			                vkCmdDraw(cmd, 3, 1, 0, 0);
		                });

		graph.AddPass("$FXAA")
		        .ReadTexture(m_ldrColor)
		        .WriteColor(graph.GetSwapchainColor(), VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, {})
		        .Execute(
		                [this, &bindless](PassContext& ctx)
		                {
			                const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

			                const VkViewport vp{
				                .x = 0.0f,
				                .y = 0.0f,
				                .width = static_cast<float>(ctx.extent.width),
				                .height = static_cast<float>(ctx.extent.height),
				                .minDepth = 0.0f,
				                .maxDepth = 1.0f,
			                };
			                const VkRect2D scissor{
				                { 0, 0 },
                                ctx.extent
			                };
			                vkCmdSetViewport(cmd, 0, 1, &vp);
			                vkCmdSetScissor(cmd, 0, 1, &scissor);

			                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline.GetPipeline());

			                const VkDescriptorSet set = bindless.GetSet();
			                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_fxaaPipeline.GetLayout(), 0, 1, &set, 0, nullptr);

			                struct
			                {
				                uint32_t ldrSlot;
				                uint32_t fxaaEnabled;
			                } push;
			                push.ldrSlot = m_ldrColorImage.GetBindlessSampledSlot();
			                push.fxaaEnabled = m_fxaaEnabled ? 1u : 0u;
			                vkCmdPushConstants(cmd, m_fxaaPipeline.GetLayout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

			                vkCmdDraw(cmd, 3, 1, 0, 0);
		                });
	}
} // namespace aether
