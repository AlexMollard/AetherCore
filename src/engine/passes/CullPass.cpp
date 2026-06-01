#include "passes/CullPass.hpp"

#include <vector>

#include "rendering/CommandRecorder.hpp"
#include "utils/Profiler.hpp"
#include "io/FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	void CullPass::Initialize(VkDevice device)
	{
		AE_PROFILE_ZONE();
		m_device = device;
	}

	void CullPass::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_device == VK_NULL_HANDLE)
		{
			return;
		}
		auto destroy = [device = m_device](VkPipeline& pipeline, VkPipelineLayout& layout)
		{
			if (pipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(device, pipeline, nullptr);
				pipeline = VK_NULL_HANDLE;
			}
			if (layout != VK_NULL_HANDLE)
			{
				vkDestroyPipelineLayout(device, layout, nullptr);
				layout = VK_NULL_HANDLE;
			}
		};
		destroy(m_singlePipeline, m_singleLayout);
		destroy(m_multiPipeline, m_multiLayout);
		m_device = VK_NULL_HANDLE;
	}

	Expected<void> CullPass::EnsureSinglePipeline()
	{
		AE_PROFILE_ZONE();
		if (m_singlePipeline != VK_NULL_HANDLE)
		{
			return {};
		}

		AE_TRY(spirv, io::FileSystem::ReadFile("shaders://cull_draws.slang.spv"));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("CullPass: shader not found: shaders://cull_draws.slang.spv"));
		}

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(m_device, *spirv, "CullPass.single"));

		const VkPushConstantRange pushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(CullContracts::PushConstants),
		};
		const VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_singleLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create single pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo stage{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shaderModule,
			.pName = "main",
		};
		const VkComputePipelineCreateInfo pipelineInfo{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = stage,
			.layout = m_singleLayout,
		};
		if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_singlePipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			vkDestroyPipelineLayout(m_device, m_singleLayout, nullptr);
			m_singleLayout = VK_NULL_HANDLE;
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create single compute pipeline."));
		}

		CommandRecorder::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(m_singlePipeline), VK_OBJECT_TYPE_PIPELINE, "CullPass.cullDraws");

		vkDestroyShaderModule(m_device, shaderModule, nullptr);
		return {};
	}

	Expected<void> CullPass::EnsureMultiPipeline()
	{
		AE_PROFILE_ZONE();
		if (m_multiPipeline != VK_NULL_HANDLE)
		{
			return {};
		}

		AE_TRY(spirv, io::FileSystem::ReadFile("shaders://cull_draws_multi.slang.spv"));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("CullPass: multi shader not found: shaders://cull_draws_multi.slang.spv"));
		}

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(m_device, *spirv, "CullPass.multi"));

		const VkPushConstantRange pushRange{
			.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
			.offset = 0,
			.size = sizeof(CullContracts::MultiPushConstants),
		};
		const VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_multiLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create multi pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo stage{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = shaderModule,
			.pName = "main",
		};
		const VkComputePipelineCreateInfo pipelineInfo{
			.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
			.stage = stage,
			.layout = m_multiLayout,
		};
		if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_multiPipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(m_device, shaderModule, nullptr);
			vkDestroyPipelineLayout(m_device, m_multiLayout, nullptr);
			m_multiLayout = VK_NULL_HANDLE;
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create multi compute pipeline."));
		}

		CommandRecorder::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(m_multiPipeline), VK_OBJECT_TYPE_PIPELINE, "CullPass.cullDrawsMulti");

		vkDestroyShaderModule(m_device, shaderModule, nullptr);
		return {};
	}

	void CullPass::RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix)
	{
		AE_PROFILE_ZONE();
		AE_EXPECT_OR_THROW_VOID(EnsureSinglePipeline());
		AE_EXPECT_OR_THROW_VOID(EnsureMultiPipeline());

		const std::string passName = namePrefix.empty() ? "$CullDraws" : ("$CullDraws_" + namePrefix);
		const VkPipeline pipeline = m_singlePipeline;
		const VkPipelineLayout layout = m_singleLayout;

		graph.AddComputePass(passName).ExecuteCompute(
		        [&renderQueue, pipeline, layout](PassContext& ctx)
		        {
			        renderQueue.PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), ctx.frameConstantsAddr, pipeline, layout, ctx.frameIndex);
		        });
	}

} // namespace aether
