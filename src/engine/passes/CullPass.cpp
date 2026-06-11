#include "passes/CullPass.hpp"

#include <vector>

#include "vulkan/VulkanUtils.hpp"
#include "utils/Profiler.hpp"
#include "io/FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	void CullPass::Initialize(gpu::Device device, gpu::PipelineCache pipelineCache)
	{
		AE_PROFILE_ZONE();
		m_device = device;
		m_pipelineCache = pipelineCache;
	}

	void CullPass::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_device == nullptr)
		{
			return;
		}
		const VkDevice vkDevice = static_cast<VkDevice>(m_device);
		auto destroy = [vkDevice](gpu::Pipeline& pipeline, gpu::PipelineLayout& layout)
		{
			const VkPipeline vkPipeline = static_cast<VkPipeline>(pipeline);
			const VkPipelineLayout vkLayout = static_cast<VkPipelineLayout>(layout);
			if (vkPipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(vkDevice, vkPipeline, nullptr);
				pipeline = nullptr;
			}
			if (vkLayout != VK_NULL_HANDLE)
			{
				vkDestroyPipelineLayout(vkDevice, vkLayout, nullptr);
				layout = nullptr;
			}
		};
		destroy(m_singlePipeline, m_singleLayout);
		destroy(m_multiPipeline, m_multiLayout);
		m_device = nullptr;
	}

	Expected<void> CullPass::EnsureSinglePipeline()
	{
		AE_PROFILE_ZONE();
		if (m_singlePipeline != nullptr)
		{
			return {};
		}

		const VkDevice vkDevice = static_cast<VkDevice>(m_device);
		const VkPipelineCache vkPipelineCache = static_cast<VkPipelineCache>(m_pipelineCache);

		AE_TRY(spirv, io::FileSystem::ReadFile("shaders://cull_draws.spv"));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("CullPass: shader not found: shaders://cull_draws.spv"));
		}

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(vkDevice, *spirv, "CullPass.single"));

		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		        .offset = 0,
		        .size = sizeof(CullContracts::PushConstants),
		};
		VkPipelineLayout vkLayout = VK_NULL_HANDLE;
		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(vkDevice, &layoutInfo, nullptr, &vkLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(vkDevice, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create single pipeline layout."));
		}
		m_singleLayout = static_cast<gpu::PipelineLayout>(vkLayout);

		const VkPipelineShaderStageCreateInfo stage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule,
		        .pName = "main",
		};
		VkPipeline vkPipeline = VK_NULL_HANDLE;
		const VkComputePipelineCreateInfo pipelineInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .stage = stage,
		        .layout = vkLayout,
		};
		if (vkCreateComputePipelines(vkDevice, vkPipelineCache, 1, &pipelineInfo, nullptr, &vkPipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(vkDevice, shaderModule, nullptr);
			vkDestroyPipelineLayout(vkDevice, vkLayout, nullptr);
			m_singleLayout = nullptr;
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create single compute pipeline."));
		}
		m_singlePipeline = static_cast<gpu::Pipeline>(vkPipeline);

		vkutil::SetObjectName(vkDevice, reinterpret_cast<std::uint64_t>(vkPipeline), VK_OBJECT_TYPE_PIPELINE, "CullPass.cullDraws");

		vkDestroyShaderModule(vkDevice, shaderModule, nullptr);
		return {};
	}

	Expected<void> CullPass::EnsureMultiPipeline()
	{
		AE_PROFILE_ZONE();
		if (m_multiPipeline != nullptr)
		{
			return {};
		}

		const VkDevice vkDevice = static_cast<VkDevice>(m_device);
		const VkPipelineCache vkPipelineCache = static_cast<VkPipelineCache>(m_pipelineCache);

		AE_TRY(spirv, io::FileSystem::ReadFile("shaders://cull_draws_multi.spv"));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("CullPass: multi shader not found: shaders://cull_draws_multi.spv"));
		}

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(vkDevice, *spirv, "CullPass.multi"));

		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		        .offset = 0,
		        .size = sizeof(CullContracts::MultiPushConstants),
		};
		VkPipelineLayout vkLayout = VK_NULL_HANDLE;
		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(vkDevice, &layoutInfo, nullptr, &vkLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(vkDevice, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create multi pipeline layout."));
		}
		m_multiLayout = static_cast<gpu::PipelineLayout>(vkLayout);

		const VkPipelineShaderStageCreateInfo stage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule,
		        .pName = "main",
		};
		VkPipeline vkPipeline = VK_NULL_HANDLE;
		const VkComputePipelineCreateInfo pipelineInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .stage = stage,
		        .layout = vkLayout,
		};
		if (vkCreateComputePipelines(vkDevice, vkPipelineCache, 1, &pipelineInfo, nullptr, &vkPipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(vkDevice, shaderModule, nullptr);
			vkDestroyPipelineLayout(vkDevice, vkLayout, nullptr);
			m_multiLayout = nullptr;
			AE_UNEXPECTED(AetherError::Vulkan(0, "CullPass: failed to create multi compute pipeline."));
		}
		m_multiPipeline = static_cast<gpu::Pipeline>(vkPipeline);

		vkutil::SetObjectName(vkDevice, reinterpret_cast<std::uint64_t>(vkPipeline), VK_OBJECT_TYPE_PIPELINE, "CullPass.cullDrawsMulti");

		vkDestroyShaderModule(vkDevice, shaderModule, nullptr);
		return {};
	}

	void CullPass::RegisterPass(RenderGraph& graph, RenderQueue& renderQueue, const std::string& namePrefix)
	{
		AE_PROFILE_ZONE();
		AE_EXPECT_OR_THROW_VOID(EnsureSinglePipeline());
		AE_EXPECT_OR_THROW_VOID(EnsureMultiPipeline());

		const std::string passName = namePrefix.empty() ? "$CullDraws" : ("$CullDraws_" + namePrefix);

		graph.AddComputePass(passName).ExecuteCompute([&renderQueue, this](PassContext& ctx) { renderQueue.PrepareAndDispatch(ctx.recorder, ctx.frameConstantsAddr, m_singlePipeline, m_singleLayout, ctx.frameIndex); });
	}
} // namespace aether
