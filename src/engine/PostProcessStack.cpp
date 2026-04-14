#include "PostProcessStack.hpp"

#include <vulkan/vulkan.h>

#include "CommandRecorder.hpp"
#include "RenderGraph.hpp"

namespace aether
{
	PostProcessStack PostProcessStack::Create(const Desc& desc)
	{
		PostProcessStack stack;

		// ── HDR color buffer ───────────────────────────────────────────────
		// The engine's forward pass renders floating-point scene colour here.
		// The tonemap pass reads it as a bindless sampled image.
		stack.m_hdrColorImage = UniqueImage::Create(desc.device, desc.allocator, {
			.extent = desc.extent,
			.format = VK_FORMAT_R16G16B16A16_SFLOAT,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			});
		stack.m_hdrColor = desc.renderGraph->RegisterImage(
			stack.m_hdrColorImage.Get(), stack.m_hdrColorImage.GetDefaultView());
		stack.m_hdrColorImage.EnsureBindlessSampled(
			*desc.bindlessManager, desc.device,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// ── LDR intermediate buffer ────────────────────────────────────────
		// Receives the Reinhard-tonemapped+gamma output (R8G8B8A8_UNORM).
		// FXAA runs on LDR data because luma-based edge detection is more
		// reliable in perceptual space than in linear floating-point.
		stack.m_ldrColorImage = UniqueImage::Create(desc.device, desc.allocator, {
			.extent = desc.extent,
			.format = VK_FORMAT_R8G8B8A8_UNORM,
			.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			});
		stack.m_ldrColor = desc.renderGraph->RegisterImage(
			stack.m_ldrColorImage.Get(), stack.m_ldrColorImage.GetDefaultView());
		stack.m_ldrColorImage.EnsureBindlessSampled(
			*desc.bindlessManager, desc.device,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// ── Pipelines ─────────────────────────────────────────────────────
		const VkDescriptorSetLayout bindlessLayout = desc.bindlessManager->GetLayout();

		// Push constant layout: uint hdrSlot + uint mode + float exposure = 12 bytes.
		stack.m_tonemapPipeline = GraphicsPipeline::Create(desc.device, {
			.shaderVfsPath = "shaders://tonemap.slang.spv",
			.colorFormat = VK_FORMAT_R8G8B8A8_UNORM,
			.noVertexInput = true,
			.pushConstantSize = 3 * sizeof(uint32_t),
			.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT,
			.setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
			});

		stack.m_fxaaPipeline = GraphicsPipeline::Create(desc.device, {
			.shaderVfsPath = "shaders://fxaa.slang.spv",
			.colorFormat = desc.swapchainFormat,
			.noVertexInput = true,
			.pushConstantSize = sizeof(uint32_t),
			.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT,
			.setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
			});

		// Tonemap pipeline that writes directly to the swapchain surface
		// (used when FXAA is disabled to avoid the extra LDR intermediate pass).
		stack.m_tonemapPipelineSwapchain = GraphicsPipeline::Create(desc.device, {
			.shaderVfsPath = "shaders://tonemap.slang.spv",
			.colorFormat = desc.swapchainFormat,
			.noVertexInput = true,
			.pushConstantSize = 3 * sizeof(uint32_t),
			.pushConstantStages = VK_SHADER_STAGE_FRAGMENT_BIT,
			.setLayouts = std::span<const VkDescriptorSetLayout>(&bindlessLayout, 1),
			});

		stack.m_swapchainFormat = desc.swapchainFormat;

		return stack;
	}

	void PostProcessStack::Destroy()
	{
		m_tonemapPipelineSwapchain.Destroy();
		m_fxaaPipeline.Destroy();
		m_tonemapPipeline.Destroy();
		m_ldrColorImage.Reset();
		m_ldrColor = RGImage{};
		m_hdrColorImage.Reset();
		m_hdrColor = RGImage{};
	}

	void PostProcessStack::RegisterPasses(RenderGraph& graph, BindlessManager& bindless)
	{
		if (!m_fxaaEnabled)
		{
			// ── FXAA disabled: tonemap writes directly to swapchain ───────────
			graph.AddPass("$PostProcess")
				.ReadTexture(m_hdrColor)
				.WriteColor(
					graph.GetSwapchainColor(),
					VK_ATTACHMENT_LOAD_OP_DONT_CARE,
					VK_ATTACHMENT_STORE_OP_STORE,
					{})
				.Execute([this, &bindless](PassContext& ctx)
					{
						const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();
						const VkViewport vp{
							.x = 0.0f, .y = 0.0f,
							.width = static_cast<float>(ctx.extent.width),
							.height = static_cast<float>(ctx.extent.height),
							.minDepth = 0.0f, .maxDepth = 1.0f,
						};
						const VkRect2D scissor{ {0, 0}, ctx.extent };
						vkCmdSetViewport(cmd, 0, 1, &vp);
						vkCmdSetScissor(cmd, 0, 1, &scissor);
						vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							m_tonemapPipelineSwapchain.GetPipeline());
						const VkDescriptorSet set = bindless.GetSet();
						vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							m_tonemapPipelineSwapchain.GetLayout(), 0, 1, &set, 0, nullptr);
						struct { uint32_t hdrSlot; uint32_t mode; float exposure; } push;
						push.hdrSlot  = m_hdrColorImage.GetBindlessSampledSlot();
						push.mode     = static_cast<uint32_t>(m_tonemapMode);
						push.exposure = m_exposure;
						vkCmdPushConstants(cmd, m_tonemapPipelineSwapchain.GetLayout(),
							VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
						vkCmdDraw(cmd, 3, 1, 0, 0);
					});
			return;
		}

		// ── FXAA enabled: tonemap → LDR intermediate, then FXAA → swapchain ─
		// ── $PostProcess — Reinhard tonemap: HDR → LDR ────────────────────
		graph.AddPass("$PostProcess")
			.ReadTexture(m_hdrColor)
			.WriteColor(
				m_ldrColor,
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_STORE,
				{})
			.Execute([this, &bindless](PassContext& ctx)
				{
					const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

					const VkViewport vp{
						.x = 0.0f, .y = 0.0f,
						.width = static_cast<float>(ctx.extent.width),
						.height = static_cast<float>(ctx.extent.height),
						.minDepth = 0.0f, .maxDepth = 1.0f,
					};
					const VkRect2D scissor{ {0, 0}, ctx.extent };
					vkCmdSetViewport(cmd, 0, 1, &vp);
					vkCmdSetScissor(cmd, 0, 1, &scissor);

					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						m_tonemapPipeline.GetPipeline());

					const VkDescriptorSet set = bindless.GetSet();
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						m_tonemapPipeline.GetLayout(), 0, 1, &set, 0, nullptr);

					struct { uint32_t hdrSlot; uint32_t mode; float exposure; } push;
					push.hdrSlot = m_hdrColorImage.GetBindlessSampledSlot();
					push.mode = static_cast<uint32_t>(m_tonemapMode);
					push.exposure = m_exposure;
					vkCmdPushConstants(cmd, m_tonemapPipeline.GetLayout(),
						VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);

					vkCmdDraw(cmd, 3, 1, 0, 0);
				});

		// ── $FXAA — fast approximate anti-aliasing: LDR → swapchain ──────
		graph.AddPass("$FXAA")
			.ReadTexture(m_ldrColor)
			.WriteColor(
				graph.GetSwapchainColor(),
				VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_STORE,
				{})
			.Execute([this, &bindless](PassContext& ctx)
				{
					const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

					const VkViewport vp{
						.x = 0.0f, .y = 0.0f,
						.width = static_cast<float>(ctx.extent.width),
						.height = static_cast<float>(ctx.extent.height),
						.minDepth = 0.0f, .maxDepth = 1.0f,
					};
					const VkRect2D scissor{ {0, 0}, ctx.extent };
					vkCmdSetViewport(cmd, 0, 1, &vp);
					vkCmdSetScissor(cmd, 0, 1, &scissor);

					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						m_fxaaPipeline.GetPipeline());

					const VkDescriptorSet set = bindless.GetSet();
					vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						m_fxaaPipeline.GetLayout(), 0, 1, &set, 0, nullptr);

					const uint32_t ldrSlot = m_ldrColorImage.GetBindlessSampledSlot();
					vkCmdPushConstants(cmd, m_fxaaPipeline.GetLayout(),
						VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(uint32_t), &ldrSlot);

					vkCmdDraw(cmd, 3, 1, 0, 0);
				});
	}
}
