#include "passes/CullPass.hpp"

#include <vector>

#include "rendering/CommandRecorder.hpp"
#include "FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	void CullPass::Initialize(VkDevice device)
	{
		m_device = device;
	}

	void CullPass::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		if (m_pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(m_device, m_pipeline, nullptr);
			m_pipeline = VK_NULL_HANDLE;
		}
		if (m_pipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
			m_pipelineLayout = VK_NULL_HANDLE;
		}
		m_device = VK_NULL_HANDLE;
	}

	Expected<void> CullPass::EnsurePipeline()
	{
		if (m_pipeline != VK_NULL_HANDLE)
		{
			return {};
		}

		AE_TRY(spirv, io::FileSystem::ReadFile("shaders://cull_draws.slang.spv"));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("CullPass: shader not found: shaders://cull_draws.slang.spv"));
		}

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(m_device, *spirv, "CullPass"));

		const VkPushConstantRange pushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(CullPushConstants),
		};
		const VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo stage{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shaderModule,
			.pName = "main", // slangc renames all entry points to "main" in SPIR-V output
		};
		const VkComputePipelineCreateInfo pipelineInfo{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = stage,
			.layout = m_pipelineLayout,
		};
		if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
			m_pipelineLayout = VK_NULL_HANDLE;
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create compute pipeline."));
		}

		CommandRecorder::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(m_pipeline), VK_OBJECT_TYPE_PIPELINE, "CullPass.cullDraws");

		vkDestroyShaderModule(m_device, shaderModule, nullptr);
		return {};
	}

	void CullPass::RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix)
	{
		AE_EXPECT_OR_THROW_VOID(EnsurePipeline());

		const std::string passName = namePrefix.empty() ? "$CullDraws" : ("$CullDraws_" + namePrefix);
		const VkPipeline pipeline = m_pipeline;
		const VkPipelineLayout layout = m_pipelineLayout;

		graph.AddComputePass(passName).ExecuteCompute(
		        [&renderQueue, pipeline, layout](PassContext& ctx)
		        {
			        // Draw commands were pre-populated by the game thread via
			        // AetherCore::GatherRenderDraws() before this frame was dispatched.
			        renderQueue.PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), ctx.frameConstantsAddr, pipeline, layout, ctx.frameIndex);
		        });
	}
} // namespace aether
