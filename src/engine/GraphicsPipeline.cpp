#include "GraphicsPipeline.hpp"

#include <stdexcept>
#include <string>
#include <cstddef>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "DrawPushConstants.hpp"
#include "FileSystem.hpp"
#include "Logger.hpp"
#include "Mesh.hpp"

namespace meow
{
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
			{
				throw std::runtime_error("Failed to create shader module.");
			}
			return mod;
		}
	}

	GraphicsPipeline::~GraphicsPipeline()
	{
		Destroy();
	}

	GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
		: m_device(std::exchange(other.m_device, VK_NULL_HANDLE))
		, m_layout(std::exchange(other.m_layout, VK_NULL_HANDLE))
		, m_pipeline(std::exchange(other.m_pipeline, VK_NULL_HANDLE))
	{}

	GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();
			m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
			m_layout = std::exchange(other.m_layout, VK_NULL_HANDLE);
			m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
		}
		return *this;
	}

	void GraphicsPipeline::Destroy()
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
		if (m_layout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(m_device, m_layout, nullptr);
			m_layout = VK_NULL_HANDLE;
		}
		m_device = VK_NULL_HANDLE;
	}

	GraphicsPipeline GraphicsPipeline::Create(VkDevice device, const Desc& desc)
	{
		const auto spirv = io::FileSystem::ReadFile(desc.shaderVfsPath);
		if (spirv.empty())
		{
			throw std::runtime_error(
				"GraphicsPipeline: shader not found: " + std::string(desc.shaderVfsPath));
		}

		VkShaderModule shaderModule = CreateShaderModule(device, spirv);

		const std::string vertEntry(desc.vertexEntry);
		const std::string fragEntry(desc.fragmentEntry);

		const VkPipelineShaderStageCreateInfo stages[2] = {
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = shaderModule,
				.pName = vertEntry.c_str(),
			},
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = shaderModule,
				.pName = fragEntry.c_str(),
			},
		};

		constexpr VkVertexInputBindingDescription kVertexBinding{
			.binding = 0,
			.stride = sizeof(Mesh::Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};
		constexpr VkVertexInputAttributeDescription kVertexAttributes[] = {
			{   // location 0 : position
				.location = 0,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = static_cast<std::uint32_t>(offsetof(Mesh::Vertex, position)),
			},
			{   // location 1 : normal
				.location = 1,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = static_cast<std::uint32_t>(offsetof(Mesh::Vertex, normal)),
			},
			{   // location 2 : tangent (xyz + bitangent sign in w)
				.location = 2,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32A32_SFLOAT,
				.offset = static_cast<std::uint32_t>(offsetof(Mesh::Vertex, tangent)),
			},
			{   // location 3 : uv
				.location = 3,
				.binding = 0,
				.format = VK_FORMAT_R32G32_SFLOAT,
				.offset = static_cast<std::uint32_t>(offsetof(Mesh::Vertex, uv)),
			},
			{   // location 4 : color
				.location = 4,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = static_cast<std::uint32_t>(offsetof(Mesh::Vertex, color)),
			},
		};
		const VkPipelineVertexInputStateCreateInfo kEmptyVertexInput{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		};
		const VkPipelineVertexInputStateCreateInfo kMeshVertexInput{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &kVertexBinding,
			.vertexAttributeDescriptionCount = 5,
			.pVertexAttributeDescriptions = kVertexAttributes,
		};
		const VkPipelineVertexInputStateCreateInfo& vertexInput =
			desc.noVertexInput ? kEmptyVertexInput : kMeshVertexInput;
		const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};
		const VkPipelineViewportStateCreateInfo viewportState{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
			.viewportCount = 1,
			.scissorCount = 1,
		};
		const VkPipelineRasterizationStateCreateInfo rasterizer{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_NONE,
			.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
			.lineWidth = 1.0f,
		};
		const VkPipelineMultisampleStateCreateInfo multisampling{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		};
		const VkPipelineDepthStencilStateCreateInfo depthStencil{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = desc.depthTestEnable ? VK_TRUE : VK_FALSE,
			.depthWriteEnable = desc.depthWriteEnable ? VK_TRUE : VK_FALSE,
			.depthCompareOp = desc.depthCompareOp,
			.depthBoundsTestEnable = VK_FALSE,
			.stencilTestEnable = VK_FALSE,
			.minDepthBounds = 0.0f,
			.maxDepthBounds = 1.0f,
		};
		const VkPipelineColorBlendAttachmentState colorBlendAttach{
			.colorWriteMask =
				VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
				VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		const VkPipelineColorBlendStateCreateInfo colorBlend{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &colorBlendAttach,
		};
		constexpr VkDynamicState kDynamicStates[] = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
		};
		const VkPipelineDynamicStateCreateInfo dynamicState{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
			.dynamicStateCount = 2,
			.pDynamicStates = kDynamicStates,
		};

		VkPipelineLayout layout = VK_NULL_HANDLE;
		const VkPushConstantRange kModelRange{
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
			.offset = 0,
			.size = sizeof(DrawPushConstants),  // 72 bytes: mat4 model + VkDeviceAddress
		};
		const VkPushConstantRange kCustomRange{
			.stageFlags = desc.pushConstantStages,
			.offset = 0,
			.size = desc.pushConstantSize,
		};
		const bool useCustomPush = desc.pushConstantSize > 0;
		const VkPipelineLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = static_cast<std::uint32_t>(desc.setLayouts.size()),
			.pSetLayouts = desc.setLayouts.data(),
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = useCustomPush ? &kCustomRange : &kModelRange,
		};
		vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);

		const VkPipelineRenderingCreateInfo renderingInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &desc.colorFormat,
			.depthAttachmentFormat = desc.depthFormat,
		};
		const VkGraphicsPipelineCreateInfo pipelineInfo{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &renderingInfo,
			.stageCount = 2,
			.pStages = stages,
			.pVertexInputState = &vertexInput,
			.pInputAssemblyState = &inputAssembly,
			.pViewportState = &viewportState,
			.pRasterizationState = &rasterizer,
			.pMultisampleState = &multisampling,
			.pDepthStencilState = &depthStencil,
			.pColorBlendState = &colorBlend,
			.pDynamicState = &dynamicState,
			.layout = layout,
		};

		VkPipeline pipeline = VK_NULL_HANDLE;
		const VkResult result = vkCreateGraphicsPipelines(
			device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

		vkDestroyShaderModule(device, shaderModule, nullptr);

		if (result != VK_SUCCESS)
		{
			vkDestroyPipelineLayout(device, layout, nullptr);
			throw std::runtime_error("Failed to create graphics pipeline.");
		}

		GraphicsPipeline out;
		out.m_device = device;
		out.m_layout = layout;
		out.m_pipeline = pipeline;
		return out;
	}
}
