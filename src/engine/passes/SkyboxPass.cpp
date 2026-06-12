#include "passes/SkyboxPass.hpp"

#include <cstring>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/RenderGraph.hpp"
#include "vulkan/GpuEnumConversions.hpp"
namespace aether
{
	SkyboxPass SkyboxPass::Create(const Desc& desc)
	{
		SkyboxPass pass;

		// Full-screen pass: no vertex input, no depth attachment.
		// Push constant: a single uint64_t BDA pointer to FrameConstantsData.
		AE_EXPECT_OR_THROW(pipeline,
		        GraphicsPipeline::Create(desc.device,
		                desc.pipelineCache,
		                {
		                        .shaderVfsPath = "shaders://skybox.spv",
		                        .colorFormat = desc.hdrColorFormat,
		                        .depthTestEnable = false,
		                        .depthWriteEnable = false,
		                        .pushConstantSize = static_cast<uint32_t>(sizeof(uint64_t)),
		                        .pushConstantStages = gpu::ShaderStage::VertexFragment,
		                }));
		pass.m_pipeline = std::move(pipeline);

		return pass;
	}

	void SkyboxPass::Destroy()
	{
		m_pipeline.Destroy();
	}

	void SkyboxPass::RegisterPass(RenderGraph& graph, RGImage hdrColor)
	{
		graph.AddPass("$Skybox")
		        .WriteColor(hdrColor, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(0.0f, 0.0f, 0.0f, 1.0f))
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                // Phase 3 migration: the pass uses gpu::CommandList
			                // (a Vulkan-free wrapper) instead of calling
			                // vkCmd* directly. The CommandList is constructed
			                // from the same underlying VkCommandBuffer that
			                // PassContext::recorder currently exposes. When the
			                // PassContext API itself migrates (a later slice
			                // of Phase 3) the wrapper construction moves into
			                // RenderGraph and this body drops a line.
			                gpu::CommandList& cmd = ctx.recorder;

			                cmd.BindPipeline(m_pipeline.GetPipeline(), m_pipeline.GetLayout());

			                // Push the BDA of this frame's FrameConstantsData so the
			                // shader can read view/proj/sun/ambient directly.
			                const gpu::DeviceAddress frameAddr = ctx.frameConstantsAddr;
			                std::byte bytes[sizeof(gpu::DeviceAddress)];
			                std::memcpy(bytes, &frameAddr, sizeof(bytes));
			                cmd.PushConstantsRaw(m_pipeline.GetLayout(), gpu::ShaderStage::VertexFragment, 0, std::span<const std::byte>(bytes, sizeof(bytes)));

			                cmd.Draw(3);
		                });
	}
} // namespace aether
