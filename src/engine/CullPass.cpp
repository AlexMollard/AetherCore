#include "CullPass.hpp"

#include <stdexcept>
#include <vector>

#include "CommandRecorder.hpp"
#include "DrawPushConstants.hpp"
#include "FileSystem.hpp"
#include "RenderGraph.hpp"
#include "RenderQueue.hpp"
#include "Scene.hpp"
#include "World.hpp"

namespace
{
	VkShaderModule CreateShaderModule(VkDevice device, const std::vector<std::byte>& spirv)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = spirv.size();
		info.pCode = reinterpret_cast<const std::uint32_t*>(spirv.data());
		VkShaderModule mod = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
			throw std::runtime_error("CullPass: failed to create shader module.");
		return mod;
	}
} // namespace

namespace aether
{
	void CullPass::Initialize(VkDevice device)
	{
		m_device = device;
	}

	void CullPass::Shutdown()
	{
		if (m_device == VK_NULL_HANDLE)
			return;
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

	void CullPass::EnsurePipeline()
	{
		if (m_pipeline != VK_NULL_HANDLE)
			return;

		const auto spirv = io::FileSystem::ReadFile("shaders://cull_draws.slang.spv");
		if (spirv.empty())
			throw std::runtime_error("CullPass: shader not found: shaders://cull_draws.slang.spv");

		VkShaderModule shaderModule = CreateShaderModule(m_device, spirv);

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
			throw std::runtime_error("CullPass: failed to create pipeline layout.");
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
			throw std::runtime_error("CullPass: failed to create compute pipeline.");
		}

		CommandRecorder::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(m_pipeline), VK_OBJECT_TYPE_PIPELINE, "CullPass.cullDraws");

		vkDestroyShaderModule(m_device, shaderModule, nullptr);
	}

	void CullPass::RegisterPass(RenderGraph& graph, Scene& scene, World& world, RenderQueue& renderQueue, const std::string& namePrefix)
	{
		EnsurePipeline();

		const std::string passName = namePrefix.empty() ? "$CullDraws" : ("$CullDraws_" + namePrefix);
		const VkPipeline pipeline = m_pipeline;
		const VkPipelineLayout layout = m_pipelineLayout;

		graph.AddComputePass(passName).ExecuteCompute(
		        [&scene, &world, &renderQueue, pipeline, layout](PassContext& ctx)
		        {
			        scene.FlushToQueue(renderQueue);
			        world.FlushToQueue(renderQueue);
			        renderQueue.PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), ctx.frameConstantsAddr, pipeline, layout, ctx.frameIndex);
		        });
	}
} // namespace aether
