#include "rendering/GraphicsPipeline.hpp"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "io/FileSystem.hpp"
#include "rendering/GpuContracts.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{

	GraphicsPipeline::~GraphicsPipeline()
	{
		Destroy();
	}

	GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
	      : m_device(std::exchange(other.m_device, nullptr)),
	        m_layout(std::exchange(other.m_layout, nullptr)),
	        m_pipeline(std::exchange(other.m_pipeline, nullptr)),
	        m_vertInputLib(std::exchange(other.m_vertInputLib, nullptr)),
	        m_preRasterLib(std::exchange(other.m_preRasterLib, nullptr)),
	        m_fragShaderLib(std::exchange(other.m_fragShaderLib, nullptr)),
	        m_fragOutputLib(std::exchange(other.m_fragOutputLib, nullptr)),
	        m_setLayoutCount(std::exchange(other.m_setLayoutCount, 0))
	{
	}

	GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();
			m_device = std::exchange(other.m_device, nullptr);
			m_layout = std::exchange(other.m_layout, nullptr);
			m_pipeline = std::exchange(other.m_pipeline, nullptr);
			m_vertInputLib = std::exchange(other.m_vertInputLib, nullptr);
			m_preRasterLib = std::exchange(other.m_preRasterLib, nullptr);
			m_fragShaderLib = std::exchange(other.m_fragShaderLib, nullptr);
			m_fragOutputLib = std::exchange(other.m_fragOutputLib, nullptr);
			m_setLayoutCount = std::exchange(other.m_setLayoutCount, 0);
		}
		return *this;
	}

	void GraphicsPipeline::Destroy()
	{
		if (m_device == nullptr)
		{
			return;
		}
		const VkDevice vkDevice = static_cast<VkDevice>(m_device);
		if (m_pipeline != nullptr)
		{
			vkDestroyPipeline(vkDevice, static_cast<VkPipeline>(m_pipeline), nullptr);
			m_pipeline = nullptr;
		}
		if (m_vertInputLib != nullptr)
		{
			vkDestroyPipeline(vkDevice, static_cast<VkPipeline>(m_vertInputLib), nullptr);
			m_vertInputLib = nullptr;
		}
		if (m_preRasterLib != nullptr)
		{
			vkDestroyPipeline(vkDevice, static_cast<VkPipeline>(m_preRasterLib), nullptr);
			m_preRasterLib = nullptr;
		}
		if (m_fragShaderLib != nullptr)
		{
			vkDestroyPipeline(vkDevice, static_cast<VkPipeline>(m_fragShaderLib), nullptr);
			m_fragShaderLib = nullptr;
		}
		if (m_fragOutputLib != nullptr)
		{
			vkDestroyPipeline(vkDevice, static_cast<VkPipeline>(m_fragOutputLib), nullptr);
			m_fragOutputLib = nullptr;
		}
		if (m_layout != nullptr)
		{
			vkDestroyPipelineLayout(vkDevice, static_cast<VkPipelineLayout>(m_layout), nullptr);
			m_layout = nullptr;
		}
		m_device = nullptr;
		m_setLayoutCount = 0;
	}

	Expected<GraphicsPipeline> GraphicsPipeline::Create(gpu::Device gpuDevice, gpu::PipelineCache gpuPipelineCache, const Desc& desc)
	{
		const VkDevice device = static_cast<VkDevice>(gpuDevice);
		const VkPipelineCache pipelineCache = static_cast<VkPipelineCache>(gpuPipelineCache);
		const VkFormat vkColorFormat = gpu::ToVk(desc.colorFormat);
		const VkFormat vkDepthFormat = gpu::ToVk(desc.depthFormat);
		const VkCompareOp vkDepthCompareOp = gpu::ToVk(desc.depthCompareOp);
		const VkShaderStageFlags vkPushConstantStages = gpu::ToVk(desc.pushConstantStages);
		const bool hasColorAttachment = vkColorFormat != VK_FORMAT_UNDEFINED;

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
		        .depthCompareOp = vkDepthCompareOp,
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
		        .attachmentCount = hasColorAttachment ? 1u : 0u,
		        .pAttachments = hasColorAttachment ? &colorBlendAttach : nullptr,
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
		        .stageFlags = vkPushConstantStages,
		        .offset = 0,
		        .size = desc.pushConstantSize,
		};
		const bool useCustomPush = desc.pushConstantSize > 0;
		std::vector<VkDescriptorSetLayout> vkSetLayouts;
		vkSetLayouts.reserve(desc.setLayouts.size());
		for (const gpu::DescriptorSetLayout setLayout: desc.setLayouts)
		{
			vkSetLayouts.push_back(static_cast<VkDescriptorSetLayout>(setLayout));
		}
		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .setLayoutCount = static_cast<std::uint32_t>(vkSetLayouts.size()),
		        .pSetLayouts = vkSetLayouts.data(),
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = useCustomPush ? &kCustomRange : &kModelRange,
		};
		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			AE_UNEXPECTED(AetherError::Vulkan(0, "Failed to create pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo vertStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_VERTEX_BIT,
		        .module = shaderModule,
		        .pName = vertEntry.c_str(),
		};
		const VkPipelineShaderStageCreateInfo fragStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		        .module = shaderModule,
		        .pName = fragEntry.c_str(),
		};

		const uint32_t colorAttachmentCount = hasColorAttachment ? 1u : 0u;
		const VkFormat* pColorFormats = hasColorAttachment ? &vkColorFormat : nullptr;

		VkPipeline pipeline = VK_NULL_HANDLE;
		VkPipeline vertInputLib = VK_NULL_HANDLE;
		VkPipeline preRasterLib = VK_NULL_HANDLE;
		VkPipeline fragShaderLib = VK_NULL_HANDLE;
		VkPipeline fragOutputLib = VK_NULL_HANDLE;

		{
			AE_ASSERT(layout != VK_NULL_HANDLE, "GraphicsPipeline: layout must be valid for GPL creation.");

			// ── GPL: vertex input interface library ──────────────────────────────
			const VkGraphicsPipelineLibraryCreateInfoEXT gplVertexInput{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
			        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
			};
			const VkGraphicsPipelineCreateInfo vertInputLibInfo{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			        .pNext = &gplVertexInput,
			        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
			        .pVertexInputState = &vertexInput,
			        .pInputAssemblyState = &inputAssembly,
			        .layout = layout,
			};
			vertInputLib = VK_NULL_HANDLE;
			VkResult result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &vertInputLibInfo, nullptr, &vertInputLib);
			if (result != VK_SUCCESS)
			{
				vkDestroyPipelineLayout(device, layout, nullptr);
				vkDestroyShaderModule(device, shaderModule, nullptr);
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create vertex-input GPL library."));
			}

			// ── GPL: pre-rasterization library (vertex stage) ────────────────────
			const VkGraphicsPipelineLibraryCreateInfoEXT gplPreRaster{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
			        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
			};
			const VkGraphicsPipelineCreateInfo preRasterLibInfo{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			        .pNext = &gplPreRaster,
			        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
			        .stageCount = 1,
			        .pStages = &vertStage,
			        .pInputAssemblyState = &inputAssembly,
			        .pViewportState = &viewportState,
			        .pRasterizationState = &rasterizer,
			        .pDynamicState = &dynamicState,
			        .layout = layout,
			};
			preRasterLib = VK_NULL_HANDLE;
			result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &preRasterLibInfo, nullptr, &preRasterLib);
			if (result != VK_SUCCESS)
			{
				vkDestroyPipeline(device, vertInputLib, nullptr);
				vkDestroyPipelineLayout(device, layout, nullptr);
				vkDestroyShaderModule(device, shaderModule, nullptr);
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create pre-rasterization GPL library."));
			}

			// ── GPL: fragment shader library ─────────────────────────────────────
			const VkPipelineRenderingCreateInfo fragShaderRenderingInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			        .colorAttachmentCount = colorAttachmentCount,
			        .pColorAttachmentFormats = pColorFormats,
			        .depthAttachmentFormat = vkDepthFormat,
			};
			const VkGraphicsPipelineLibraryCreateInfoEXT gplFragShader{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
			        .pNext = &fragShaderRenderingInfo,
			        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
			};
			const VkGraphicsPipelineCreateInfo fragShaderLibInfo{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			        .pNext = &gplFragShader,
			        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
			        .stageCount = 1,
			        .pStages = &fragStage,
			        .pDepthStencilState = &depthStencil,
			        .layout = layout,
			};
			fragShaderLib = VK_NULL_HANDLE;
			result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &fragShaderLibInfo, nullptr, &fragShaderLib);
			if (result != VK_SUCCESS)
			{
				vkDestroyPipeline(device, preRasterLib, nullptr);
				vkDestroyPipeline(device, vertInputLib, nullptr);
				vkDestroyPipelineLayout(device, layout, nullptr);
				vkDestroyShaderModule(device, shaderModule, nullptr);
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create fragment-shader GPL library."));
			}

			// ── GPL: fragment output interface library ───────────────────────────
			const VkPipelineRenderingCreateInfo fragOutputRenderingInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			        .colorAttachmentCount = colorAttachmentCount,
			        .pColorAttachmentFormats = pColorFormats,
			        .depthAttachmentFormat = vkDepthFormat,
			};
			const VkGraphicsPipelineLibraryCreateInfoEXT gplFragOutput{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
			        .pNext = &fragOutputRenderingInfo,
			        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
			};
			const VkGraphicsPipelineCreateInfo fragOutputLibInfo{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			        .pNext = &gplFragOutput,
			        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT,
			        .pMultisampleState = &multisampling,
			        .pColorBlendState = &colorBlend,
			        .layout = layout,
			};
			fragOutputLib = VK_NULL_HANDLE;
			result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &fragOutputLibInfo, nullptr, &fragOutputLib);
			if (result != VK_SUCCESS)
			{
				vkDestroyPipeline(device, fragShaderLib, nullptr);
				vkDestroyPipeline(device, preRasterLib, nullptr);
				vkDestroyPipeline(device, vertInputLib, nullptr);
				vkDestroyPipelineLayout(device, layout, nullptr);
				vkDestroyShaderModule(device, shaderModule, nullptr);
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to create fragment-output GPL library."));
			}

			// ── GPL: link all libraries into final pipeline ──────────────────────
			const VkPipeline kLibs[] = {vertInputLib, preRasterLib, fragShaderLib, fragOutputLib};
			const VkPipelineLibraryCreateInfoKHR libLink{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
			        .libraryCount = 4,
			        .pLibraries = kLibs,
			};
			const VkPipelineRenderingCreateInfo renderingInfo{
			        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			        .pNext = &libLink,
			        .colorAttachmentCount = colorAttachmentCount,
			        .pColorAttachmentFormats = pColorFormats,
			        .depthAttachmentFormat = vkDepthFormat,
			};
			const VkGraphicsPipelineCreateInfo linkInfo{
			        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			        .pNext = &renderingInfo,
			        .flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT,
			        .layout = layout,
			};

			result = vkCreateGraphicsPipelines(device, pipelineCache, 1, &linkInfo, nullptr, &pipeline);

			if (result != VK_SUCCESS)
			{
				vkDestroyPipelineLayout(device, layout, nullptr);
				vkDestroyShaderModule(device, shaderModule, nullptr);
				AE_UNEXPECTED(AetherError::Vulkan(static_cast<int32_t>(result), "Failed to link GPL pipeline."));
			}
		}

		vkDestroyShaderModule(device, shaderModule, nullptr);

		GraphicsPipeline out;
		out.m_device = device;
		out.m_layout = layout;
		out.m_pipeline = pipeline;
		out.m_vertInputLib = vertInputLib;
		out.m_preRasterLib = preRasterLib;
		out.m_fragShaderLib = fragShaderLib;
		out.m_fragOutputLib = fragOutputLib;
		out.m_setLayoutCount = static_cast<std::uint32_t>(desc.setLayouts.size());
		return out;
	}
} // namespace aether
