#include "rendering/GraphicsPipeline.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "io/FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{

	GraphicsPipeline::~GraphicsPipeline()
	{
		Destroy();
	}

	GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
	      : m_device(std::exchange(other.m_device, VK_NULL_HANDLE)), m_layout(std::exchange(other.m_layout, VK_NULL_HANDLE)), m_pipeline(std::exchange(other.m_pipeline, VK_NULL_HANDLE)), m_setLayoutCount(std::exchange(other.m_setLayoutCount, 0))
	{
	}

	GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();
			m_device = std::exchange(other.m_device, VK_NULL_HANDLE);
			m_layout = std::exchange(other.m_layout, VK_NULL_HANDLE);
			m_pipeline = std::exchange(other.m_pipeline, VK_NULL_HANDLE);
			m_setLayoutCount = std::exchange(other.m_setLayoutCount, 0);
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
		m_setLayoutCount = 0;
	}

	Expected<GraphicsPipeline> GraphicsPipeline::Create(VkDevice device, VkPipelineCache pipelineCache, const Desc& desc)
	{
		AE_TRY(spirv, io::FileSystem::ReadFile(desc.shaderVfsPath));
		if (spirv->empty())
		{
			AE_UNEXPECTED(AetherError::Asset("GraphicsPipeline: shader not found: " + std::string(desc.shaderVfsPath)));
		}

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, *spirv, "GraphicsPipeline"));

		const std::string vertEntry(desc.vertexEntry);
		const std::string fragEntry(desc.fragmentEntry);

		// ── Common graphics state shared by vertex library and final link ────
		const VkPipelineVertexInputStateCreateInfo vertexInput{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		};
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
		        .blendEnable = desc.blendEnable ? VK_TRUE : VK_FALSE,
		        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		        .colorBlendOp = VK_BLEND_OP_ADD,
		        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		        .alphaBlendOp = VK_BLEND_OP_ADD,
		        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};
		const VkPipelineColorBlendStateCreateInfo colorBlend{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		        .attachmentCount = desc.colorFormat != VK_FORMAT_UNDEFINED ? 1u : 0u,
		        .pAttachments = desc.colorFormat != VK_FORMAT_UNDEFINED ? &colorBlendAttach : nullptr,
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

		// ── Pipeline layout ──────────────────────────────────────────────────
		VkPipelineLayout layout = VK_NULL_HANDLE;
		const VkPushConstantRange kModelRange{
		        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		        .offset = 0,
		        .size = sizeof(DrawContracts::PushConstants),
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
		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "Failed to create pipeline layout."));
		}

		// ── GPL: pre-rasterization library (vertex stage) ────────────────────
		const VkPipelineShaderStageCreateInfo vertStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_VERTEX_BIT,
		        .module = shaderModule,
		        .pName = vertEntry.c_str(),
		};
		const VkGraphicsPipelineCreateInfo vertLibInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
		        .stageCount = 1,
		        .pStages = &vertStage,
		        .pVertexInputState = &vertexInput,
		        .pInputAssemblyState = &inputAssembly,
		        .pViewportState = &viewportState,
		        .pRasterizationState = &rasterizer,
		        .pDynamicState = &dynamicState,
		        .layout = layout,
		};
		VkPipeline vertLib = VK_NULL_HANDLE;
		VkResult result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &vertLibInfo, nullptr, &vertLib);
		if (result != VK_SUCCESS)
		{
			vkDestroyPipelineLayout(device, layout, nullptr);
			vkDestroyShaderModule(device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create vertex GPL library."));
		}

		// ── GPL: fragment shader library ─────────────────────────────────────
		const VkPipelineShaderStageCreateInfo fragStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .module = shaderModule,
		        .pName = fragEntry.c_str(),
		};
		const VkGraphicsPipelineCreateInfo fragLibInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
		        .stageCount = 1,
		        .pStages = &fragStage,
		        .pDepthStencilState = &depthStencil,
		        .pColorBlendState = &colorBlend,
		        .pMultisampleState = &multisampling,
		        .layout = layout,
		};
		VkPipeline fragLib = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &fragLibInfo, nullptr, &fragLib);
		if (result != VK_SUCCESS)
		{
			vkDestroyPipeline(device, vertLib, nullptr);
			vkDestroyPipelineLayout(device, layout, nullptr);
			vkDestroyShaderModule(device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create fragment GPL library."));
		}

		// ── GPL: link vertex + fragment libraries into final pipeline ────────
		const VkPipeline kLibs[] = {vertLib, fragLib};
		const VkPipelineLibraryCreateInfoKHR libLink{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
		        .libraryCount = 2,
		        .pLibraries = kLibs,
		};
		const bool hasColorAttachment = desc.colorFormat != VK_FORMAT_UNDEFINED;
		const VkPipelineRenderingCreateInfo renderingInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		        .pNext = &libLink,
		        .colorAttachmentCount = hasColorAttachment ? 1u : 0u,
		        .pColorAttachmentFormats = hasColorAttachment ? &desc.colorFormat : nullptr,
		        .depthAttachmentFormat = desc.depthFormat,
		};
		const VkGraphicsPipelineCreateInfo linkInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &renderingInfo,
		        .layout = layout,
		};

		VkPipeline pipeline = VK_NULL_HANDLE;
		result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &linkInfo, nullptr, &pipeline);

		vkDestroyShaderModule(device, shaderModule, nullptr);

		if (result != VK_SUCCESS)
		{
			vkDestroyPipeline(device, vertLib, nullptr);
			vkDestroyPipeline(device, fragLib, nullptr);
			vkDestroyPipelineLayout(device, layout, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to link GPL pipeline."));
		}

		// Library pipelines served their purpose — no longer needed after linking.
		vkDestroyPipeline(device, vertLib, nullptr);
		vkDestroyPipeline(device, fragLib, nullptr);

		GraphicsPipeline out;
		out.m_device = device;
		out.m_layout = layout;
		out.m_pipeline = pipeline;
		out.m_setLayoutCount = static_cast<std::uint32_t>(desc.setLayouts.size());
		return out;
	}
} // namespace aether
