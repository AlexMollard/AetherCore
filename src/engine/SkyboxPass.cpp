#include "SkyboxPass.hpp"

#include <vulkan/vulkan.h>

#include "CommandRecorder.hpp"
#include "RenderGraph.hpp"

namespace aether
{
	SkyboxPass SkyboxPass::Create(const Desc& desc)
	{
		SkyboxPass pass;

		// Full-screen pass: no vertex input, no depth attachment.
		// Push constant: a single uint64_t BDA pointer to FrameConstantsData.
		pass.m_pipeline = GraphicsPipeline::Create(desc.device, {
			.shaderVfsPath     = "shaders://skybox.slang.spv",
			.colorFormat       = desc.hdrColorFormat,
			.depthTestEnable   = false,
			.depthWriteEnable  = false,
			.noVertexInput     = true,
			.pushConstantSize  = static_cast<uint32_t>(sizeof(uint64_t)),
			.pushConstantStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		});

		return pass;
	}

	void SkyboxPass::Destroy()
	{
		m_pipeline.Destroy();
	}

	void SkyboxPass::RegisterPass(RenderGraph& graph, RGImage hdrColor)
	{
		graph.AddPass("$Skybox")
			.WriteColor(
				hdrColor,
				VK_ATTACHMENT_LOAD_OP_CLEAR,
				VK_ATTACHMENT_STORE_OP_STORE,
				ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f))
			.Execute([this](PassContext& ctx)
				{
					const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

					vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						m_pipeline.GetPipeline());

					// Push the BDA of this frame's FrameConstantsData so the
					// shader can read view/proj/sun/ambient directly.
					vkCmdPushConstants(cmd, m_pipeline.GetLayout(),
						VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
						0, sizeof(uint64_t), &ctx.frameConstantsAddr);

					vkCmdDraw(cmd, 3, 1, 0, 0);
				});
	}
}
