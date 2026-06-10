#include "physics/PhysicsDebugRenderer.hpp"

#include <array>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>

#include "rendering/RenderGraph.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "utils/Logger.hpp"
#include "scene/World.hpp"
#include "scene/System.hpp"
#include "physics/PhysicsComponents.hpp"
#include "io/FileSystem.hpp"

using namespace aether::vkutil;

namespace aether
{
	namespace
	{
		glm::vec4 GetColorForMotionType(const PhysicsMotionType motionType)
		{
			switch (motionType)
			{
				case PhysicsMotionType::Static:
					return glm::vec4(1.0f, 0.2f, 0.2f, 1.0f); // Red
				case PhysicsMotionType::Kinematic:
					return glm::vec4(0.2f, 1.0f, 0.2f, 1.0f); // Green
				case PhysicsMotionType::Dynamic:
					return glm::vec4(0.2f, 0.4f, 1.0f, 1.0f); // Blue
			}
			return glm::vec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow fallback
		}
	} // namespace

	static bool s_debugRenderingEnabled = true; // on by default; F6 toggles from DebugLayer

	void SetPhysicsDebugRenderingEnabled(bool enabled)
	{
		s_debugRenderingEnabled = enabled;
	}

	bool IsPhysicsDebugRenderingEnabled()
	{
		return s_debugRenderingEnabled;
	}

	PhysicsDebugRenderer::~PhysicsDebugRenderer()
	{
		if (m_device != VK_NULL_HANDLE)
		{
			Shutdown(m_device);
		}
	}

	PhysicsDebugRenderer::PhysicsDebugRenderer(PhysicsDebugRenderer&& rhs) noexcept
	      : m_device(rhs.m_device),
	        m_allocator(rhs.m_allocator),
	        m_enabled(rhs.m_enabled),
	        m_selfTestEnabled(rhs.m_selfTestEnabled),
	        m_colorMode(rhs.m_colorMode),
	        m_world(rhs.m_world),
	        m_colorFormat(rhs.m_colorFormat),
	        m_depthFormat(rhs.m_depthFormat),
	        m_pipeline(rhs.m_pipeline),
	        m_pipelineLayout(rhs.m_pipelineLayout),
	        m_boxVertexBuffer(rhs.m_boxVertexBuffer),
	        m_boxVertexAlloc(rhs.m_boxVertexAlloc),
	        m_boxVertexCount(rhs.m_boxVertexCount),
	        m_sphereVertexBuffer(rhs.m_sphereVertexBuffer),
	        m_sphereVertexAlloc(rhs.m_sphereVertexAlloc),
	        m_sphereVertexCount(rhs.m_sphereVertexCount),
	        m_capsuleVertexBuffer(rhs.m_capsuleVertexBuffer),
	        m_capsuleVertexAlloc(rhs.m_capsuleVertexAlloc),
	        m_capsuleVertexCount(rhs.m_capsuleVertexCount),
	        m_immediateVertexBuffer(rhs.m_immediateVertexBuffer),
	        m_immediateVertexAlloc(rhs.m_immediateVertexAlloc),
	        m_immediateCapacity(rhs.m_immediateCapacity)
	{
		rhs.m_device = VK_NULL_HANDLE;
		rhs.m_pipeline = VK_NULL_HANDLE;
		rhs.m_pipelineLayout = VK_NULL_HANDLE;
		rhs.m_boxVertexBuffer = VK_NULL_HANDLE;
		rhs.m_boxVertexAlloc = VK_NULL_HANDLE;
		rhs.m_sphereVertexBuffer = VK_NULL_HANDLE;
		rhs.m_sphereVertexAlloc = VK_NULL_HANDLE;
		rhs.m_capsuleVertexBuffer = VK_NULL_HANDLE;
		rhs.m_capsuleVertexAlloc = VK_NULL_HANDLE;
		rhs.m_immediateVertexBuffer = VK_NULL_HANDLE;
		rhs.m_immediateVertexAlloc = VK_NULL_HANDLE;
		rhs.m_immediateCapacity = 0;
	}

	PhysicsDebugRenderer& PhysicsDebugRenderer::operator=(PhysicsDebugRenderer&& rhs) noexcept
	{
		if (this != &rhs)
		{
			Shutdown(rhs.m_device);
			m_device = rhs.m_device;
			m_allocator = rhs.m_allocator;
			m_enabled = rhs.m_enabled;
			m_selfTestEnabled = rhs.m_selfTestEnabled;
			m_colorMode = rhs.m_colorMode;
			m_world = rhs.m_world;
			m_colorFormat = rhs.m_colorFormat;
			m_depthFormat = rhs.m_depthFormat;
			m_pipeline = rhs.m_pipeline;
			m_pipelineLayout = rhs.m_pipelineLayout;
			m_boxVertexBuffer = rhs.m_boxVertexBuffer;
			m_boxVertexAlloc = rhs.m_boxVertexAlloc;
			m_boxVertexCount = rhs.m_boxVertexCount;
			m_sphereVertexBuffer = rhs.m_sphereVertexBuffer;
			m_sphereVertexAlloc = rhs.m_sphereVertexAlloc;
			m_sphereVertexCount = rhs.m_sphereVertexCount;
			m_capsuleVertexBuffer = rhs.m_capsuleVertexBuffer;
			m_capsuleVertexAlloc = rhs.m_capsuleVertexAlloc;
			m_capsuleVertexCount = rhs.m_capsuleVertexCount;
			m_immediateVertexBuffer = rhs.m_immediateVertexBuffer;
			m_immediateVertexAlloc = rhs.m_immediateVertexAlloc;
			m_immediateCapacity = rhs.m_immediateCapacity;

			rhs.m_device = VK_NULL_HANDLE;
			rhs.m_pipeline = VK_NULL_HANDLE;
			rhs.m_pipelineLayout = VK_NULL_HANDLE;
			rhs.m_boxVertexBuffer = VK_NULL_HANDLE;
			rhs.m_boxVertexAlloc = VK_NULL_HANDLE;
			rhs.m_sphereVertexBuffer = VK_NULL_HANDLE;
			rhs.m_sphereVertexAlloc = VK_NULL_HANDLE;
			rhs.m_capsuleVertexBuffer = VK_NULL_HANDLE;
			rhs.m_capsuleVertexAlloc = VK_NULL_HANDLE;
			rhs.m_immediateVertexBuffer = VK_NULL_HANDLE;
			rhs.m_immediateVertexAlloc = VK_NULL_HANDLE;
			rhs.m_immediateCapacity = 0;
		}
		return *this;
	}

	void PhysicsDebugRenderer::Init(VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat)
	{
		m_device = ctx.GetDevice().device;
		m_allocator = ctx.GetAllocator();
		m_colorFormat = colorFormat;
		m_depthFormat = depthFormat;
		CreateWireframePipeline(ctx, colorFormat, depthFormat);
		CreateBoxGeometry(ctx.GetAllocator());
		CreateSphereGeometry(ctx.GetAllocator());
		CreateCapsuleGeometry(ctx.GetAllocator());
		m_immediateCapacity = 0; // lazy-allocate on first frame
		m_enabled = true;
		m_colorMode = PhysicsDebugColorMode::ByMotionType;
	}

	void PhysicsDebugRenderer::Shutdown(VkDevice device)
	{
		if (m_boxVertexBuffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_allocator, m_boxVertexBuffer, m_boxVertexAlloc);
			m_boxVertexBuffer = VK_NULL_HANDLE;
		}
		if (m_sphereVertexBuffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_allocator, m_sphereVertexBuffer, m_sphereVertexAlloc);
			m_sphereVertexBuffer = VK_NULL_HANDLE;
		}
		if (m_capsuleVertexBuffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(m_allocator, m_capsuleVertexBuffer, m_capsuleVertexAlloc);
			m_capsuleVertexBuffer = VK_NULL_HANDLE;
		}
		DestroyImmediateBuffer(m_allocator);
		if (m_pipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, m_pipeline, nullptr);
			m_pipeline = VK_NULL_HANDLE;
		}
		if (m_pipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, m_pipelineLayout, nullptr);
			m_pipelineLayout = VK_NULL_HANDLE;
		}
		m_immediateCapacity = 0;
	}

	void PhysicsDebugRenderer::CreateWireframePipeline(VulkanContext& ctx, VkFormat colorFormat, VkFormat depthFormat)
	{
		auto vertData = io::FileSystem::ReadFile("shaders://debug_vert.spv");
		auto fragData = io::FileSystem::ReadFile("shaders://debug_frag.spv");

		if (!vertData.has_value() || !fragData.has_value())
		{
			AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to load shader files");
			return;
		}

		VkShaderModule vertModule = VK_NULL_HANDLE;
		VkShaderModule fragModule = VK_NULL_HANDLE;

		{
			const VkShaderModuleCreateInfo info{
			        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			        .codeSize = vertData->size(),
			        .pCode = reinterpret_cast<const std::uint32_t*>(vertData->data()),
			};
			if (vkCreateShaderModule(ctx.GetDevice().device, &info, nullptr, &vertModule) != VK_SUCCESS)
			{
				AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to create vertex shader");
				return;
			}
		}

		{
			const VkShaderModuleCreateInfo info{
			        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
			        .codeSize = fragData->size(),
			        .pCode = reinterpret_cast<const std::uint32_t*>(fragData->data()),
			};
			if (vkCreateShaderModule(ctx.GetDevice().device, &info, nullptr, &fragModule) != VK_SUCCESS)
			{
				AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to create fragment shader");
				vkDestroyShaderModule(ctx.GetDevice().device, vertModule, nullptr);
				return;
			}
		}

		const VkPushConstantRange kPushRange{
		        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		        .offset = 0,
		        // { uint64_t frameAddr, vec4 tintColor, mat4 model }
		        .size = sizeof(std::uint64_t) + sizeof(glm::vec4) + sizeof(glm::mat4),
		};

		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .setLayoutCount = 0,
		        .pSetLayouts = nullptr,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &kPushRange,
		};

		if (vkCreatePipelineLayout(ctx.GetDevice().device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to create pipeline layout");
			vkDestroyShaderModule(ctx.GetDevice().device, vertModule, nullptr);
			vkDestroyShaderModule(ctx.GetDevice().device, fragModule, nullptr);
			return;
		}

		const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
		        VkPipelineShaderStageCreateInfo{
		                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                .stage = VK_SHADER_STAGE_VERTEX_BIT,
		                .module = vertModule,
		                .pName = "main",
		        },
		        VkPipelineShaderStageCreateInfo{
		                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
		                .module = fragModule,
		                .pName = "main",
		        },
		};

		// Per-vertex: position (vec3) at location 0, color (vec4) at location 1.
		const VkVertexInputBindingDescription kBinding{
		        .binding = 0,
		        .stride = sizeof(DebugVertex),
		        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		const std::array<VkVertexInputAttributeDescription, 2> kAttribs{
		        VkVertexInputAttributeDescription{
		                .location = 0,
		                .binding = 0,
		                .format = VK_FORMAT_R32G32B32_SFLOAT,
		                .offset = offsetof(DebugVertex, position),
		        },
		        VkVertexInputAttributeDescription{
		                .location = 1,
		                .binding = 0,
		                .format = VK_FORMAT_R32G32B32A32_SFLOAT,
		                .offset = offsetof(DebugVertex, color),
		        },
		};

		const VkPipelineVertexInputStateCreateInfo vertexInput{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		        .vertexBindingDescriptionCount = 1,
		        .pVertexBindingDescriptions = &kBinding,
		        .vertexAttributeDescriptionCount = static_cast<std::uint32_t>(kAttribs.size()),
		        .pVertexAttributeDescriptions = kAttribs.data(),
		};

		const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		        .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
		        .primitiveRestartEnable = VK_FALSE,
		};

		const VkPipelineRasterizationStateCreateInfo rasterization{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		        .depthClampEnable = VK_FALSE,
		        .rasterizerDiscardEnable = VK_FALSE,
		        .polygonMode = VK_POLYGON_MODE_LINE,
		        .cullMode = VK_CULL_MODE_NONE,
		        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		        .depthBiasEnable = VK_FALSE,
		        .lineWidth = 1.0f,
		};

		const VkPipelineMultisampleStateCreateInfo multisample{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
		        .sampleShadingEnable = VK_FALSE,
		};

		const VkPipelineColorBlendAttachmentState colorBlend{
		        .blendEnable = VK_TRUE,
		        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		        .colorBlendOp = VK_BLEND_OP_ADD,
		        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		        .alphaBlendOp = VK_BLEND_OP_ADD,
		        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
		};

		const VkPipelineColorBlendStateCreateInfo colorBlendState{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		        .logicOpEnable = VK_FALSE,
		        .attachmentCount = 1,
		        .pAttachments = &colorBlend,
		};

		const VkPipelineDepthStencilStateCreateInfo depthStencil{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		        .depthTestEnable = VK_FALSE,
		        .depthWriteEnable = VK_FALSE,
		        .depthBoundsTestEnable = VK_FALSE,
		        .stencilTestEnable = VK_FALSE,
		};

		const VkViewport kViewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
		const VkRect2D kScissor{{0, 0}, {1, 1}};
		const VkPipelineViewportStateCreateInfo viewportState{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		        .viewportCount = 1,
		        .pViewports = &kViewport,
		        .scissorCount = 1,
		        .pScissors = &kScissor,
		};

		const VkPipelineRenderingCreateInfo renderingInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
		        .colorAttachmentCount = 1,
		        .pColorAttachmentFormats = &colorFormat,
		        .depthAttachmentFormat = depthFormat,
		};

		const VkGraphicsPipelineCreateInfo pipelineInfo{
		        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		        .pNext = &renderingInfo,
		        .stageCount = 2,
		        .pStages = std::data(stages),
		        .pVertexInputState = &vertexInput,
		        .pInputAssemblyState = &inputAssembly,
		        .pViewportState = &viewportState,
		        .pRasterizationState = &rasterization,
		        .pMultisampleState = &multisample,
		        .pDepthStencilState = &depthStencil,
		        .pColorBlendState = &colorBlendState,
		        .layout = m_pipelineLayout,
		        .renderPass = VK_NULL_HANDLE,
		        .subpass = 0,
		};

		const VkResult result = vkCreateGraphicsPipelines(ctx.GetDevice().device, ctx.GetPipelineCache(), 1, &pipelineInfo, nullptr, &m_pipeline);
		if (result != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to create pipeline, error {}", static_cast<int>(result));
		}

		vkDestroyShaderModule(ctx.GetDevice().device, vertModule, nullptr);
		vkDestroyShaderModule(ctx.GetDevice().device, fragModule, nullptr);
	}

	void PhysicsDebugRenderer::CreateBoxGeometry(VmaAllocator allocator)
	{
		const std::array<glm::vec3, 24> kBoxEdges = {
		        // Bottom face (4 edges)
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        // Top face (4 edges)
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        // Vertical edges (4 edges)
		        glm::vec3{-0.5f, -0.5f, -0.5f},
		        glm::vec3{-0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, -0.5f},
		        glm::vec3{0.5f, 0.5f, -0.5f},
		        glm::vec3{0.5f, -0.5f, 0.5f},
		        glm::vec3{0.5f, 0.5f, 0.5f},
		        glm::vec3{-0.5f, -0.5f, 0.5f},
		        glm::vec3{-0.5f, 0.5f, 0.5f},
		};

		std::array<DebugVertex, 24> vertices;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};
		for (std::size_t i = 0; i < kBoxEdges.size(); ++i)
		{
			vertices[i] = DebugVertex{kBoxEdges[i], kWhite};
		}

		m_boxVertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = sizeof(vertices),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_boxVertexBuffer, &m_boxVertexAlloc, nullptr);

		void* data = nullptr;
		vmaMapMemory(allocator, m_boxVertexAlloc, &data);
		std::memcpy(data, vertices.data(), sizeof(vertices));
		vmaUnmapMemory(allocator, m_boxVertexAlloc);
	}

	void PhysicsDebugRenderer::CreateSphereGeometry(VmaAllocator allocator)
	{
		std::vector<DebugVertex> vertices;
		vertices.reserve(64 * 3);

		constexpr int kSegments = 16;
		constexpr float kTubeRadius = 0.5f;
		constexpr float kTwoPi = 6.28318530718f;
		constexpr float kPi = 3.14159265359f;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = static_cast<float>(i * kTwoPi) / kSegments;
			const float theta2 = static_cast<float>((i + 1) * kTwoPi) / kSegments;

			for (int j = 0; j < kSegments; ++j)
			{
				const float phi1 = static_cast<float>(j * kPi) / kSegments;
				const float phi2 = static_cast<float>((j + 1) * kPi) / kSegments;

				vertices.push_back(DebugVertex{glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta1), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta1)}, kWhite});
				vertices.push_back(DebugVertex{glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta2), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta2)}, kWhite});

				vertices.push_back(DebugVertex{glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta1), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta1)}, kWhite});
				vertices.push_back(DebugVertex{glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta2), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta2)}, kWhite});
			}
		}

		m_sphereVertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = vertices.size() * sizeof(DebugVertex),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_sphereVertexBuffer, &m_sphereVertexAlloc, nullptr);

		void* data = nullptr;
		vmaMapMemory(allocator, m_sphereVertexAlloc, &data);
		std::memcpy(data, vertices.data(), vertices.size() * sizeof(DebugVertex));
		vmaUnmapMemory(allocator, m_sphereVertexAlloc);
	}

	void PhysicsDebugRenderer::CreateCapsuleGeometry(VmaAllocator allocator)
	{
		std::vector<DebugVertex> vertices;
		constexpr glm::vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

		constexpr int kSegments = 8;
		constexpr float kRadius = 0.5f;
		constexpr float kHalfHeight = 0.5f;
		constexpr float kTwoPi = 6.28318530718f;
		constexpr float kPi = 3.14159265359f;

		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = static_cast<float>(i * kTwoPi) / kSegments;
			const float theta2 = static_cast<float>((i + 1) * kTwoPi) / kSegments;

			for (int j = 0; j < kSegments / 2; ++j)
			{
				const float phi1 = static_cast<float>(j * kPi) / kSegments;
				const float phi2 = static_cast<float>((j + 1) * kPi) / kSegments;

				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi1) * std::cos(theta1), kHalfHeight + kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta1)}, kWhite});
				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi1) * std::cos(theta2), kHalfHeight + kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta2)}, kWhite});

				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi2) * std::cos(theta1), kHalfHeight + kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta1)}, kWhite});
				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi2) * std::cos(theta2), kHalfHeight + kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta2)}, kWhite});

				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi1) * std::cos(theta1), -kHalfHeight - kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta1)}, kWhite});
				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi1) * std::cos(theta2), -kHalfHeight - kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta2)}, kWhite});

				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi2) * std::cos(theta1), -kHalfHeight - kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta1)}, kWhite});
				vertices.push_back(DebugVertex{glm::vec3{kRadius * std::sin(phi2) * std::cos(theta2), -kHalfHeight - kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta2)}, kWhite});
			}
		}

		m_capsuleVertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = vertices.size() * sizeof(DebugVertex),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_capsuleVertexBuffer, &m_capsuleVertexAlloc, nullptr);

		void* data = nullptr;
		vmaMapMemory(allocator, m_capsuleVertexAlloc, &data);
		std::memcpy(data, vertices.data(), vertices.size() * sizeof(DebugVertex));
		vmaUnmapMemory(allocator, m_capsuleVertexAlloc);
	}

	// ── Free-function debug primitive builders ───────────────────────────────

	void AddDebugLine(std::vector<DebugVertex>& out, const glm::vec3& a, const glm::vec3& b, const glm::vec4& color)
	{
		out.push_back(DebugVertex{a, color});
		out.push_back(DebugVertex{b, color});
	}

	void AddDebugAabb(std::vector<DebugVertex>& out, const glm::vec3& min, const glm::vec3& max, const glm::vec4& color)
	{
		// 8 corners
		const glm::vec3 c000{min.x, min.y, min.z};
		const glm::vec3 c100{max.x, min.y, min.z};
		const glm::vec3 c010{min.x, max.y, min.z};
		const glm::vec3 c110{max.x, max.y, min.z};
		const glm::vec3 c001{min.x, min.y, max.z};
		const glm::vec3 c101{max.x, min.y, max.z};
		const glm::vec3 c011{min.x, max.y, max.z};
		const glm::vec3 c111{max.x, max.y, max.z};

		// 4 bottom
		AddDebugLine(out, c000, c100, color);
		AddDebugLine(out, c100, c101, color);
		AddDebugLine(out, c101, c001, color);
		AddDebugLine(out, c001, c000, color);
		// 4 top
		AddDebugLine(out, c010, c110, color);
		AddDebugLine(out, c110, c111, color);
		AddDebugLine(out, c111, c011, color);
		AddDebugLine(out, c011, c010, color);
		// 4 verticals
		AddDebugLine(out, c000, c010, color);
		AddDebugLine(out, c100, c110, color);
		AddDebugLine(out, c101, c111, color);
		AddDebugLine(out, c001, c011, color);
	}

	void AddDebugBox(std::vector<DebugVertex>& out, const glm::vec3& center, const glm::quat& rotation, const glm::vec3& halfExtents, const glm::vec4& color)
	{
		const glm::mat3 rot{rotation};
		const std::array<glm::vec3, 8> corners{
		        center + rot * glm::vec3{-halfExtents.x, -halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, -halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, -halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, -halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, halfExtents.y, -halfExtents.z},
		        center + rot * glm::vec3{halfExtents.x, halfExtents.y, halfExtents.z},
		        center + rot * glm::vec3{-halfExtents.x, halfExtents.y, halfExtents.z},
		};

		// 4 bottom
		AddDebugLine(out, corners[0], corners[1], color);
		AddDebugLine(out, corners[1], corners[2], color);
		AddDebugLine(out, corners[2], corners[3], color);
		AddDebugLine(out, corners[3], corners[0], color);
		// 4 top
		AddDebugLine(out, corners[4], corners[5], color);
		AddDebugLine(out, corners[5], corners[6], color);
		AddDebugLine(out, corners[6], corners[7], color);
		AddDebugLine(out, corners[7], corners[4], color);
		// 4 verticals
		AddDebugLine(out, corners[0], corners[4], color);
		AddDebugLine(out, corners[1], corners[5], color);
		AddDebugLine(out, corners[2], corners[6], color);
		AddDebugLine(out, corners[3], corners[7], color);
	}

	void AddDebugSphere(std::vector<DebugVertex>& out, const glm::vec3& center, float radius, const glm::vec4& color, int segments)
	{
		constexpr float kPi = 3.14159265359f;
		const float kTwoPi = 2.0f * kPi;
		const int clamped = segments < 4 ? 4 : segments;

		auto pointOnCircle = [&](float theta, float phi) -> glm::vec3
		{
			return center + radius * glm::vec3{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
		};

		// 3 great circles (XY, XZ, YZ planes) for a recognizable sphere outline.
		// XY plane (phi=pi/2)
		for (int i = 0; i < clamped; ++i)
		{
			const float t1 = static_cast<float>(i) * kTwoPi / clamped;
			const float t2 = static_cast<float>(i + 1) * kTwoPi / clamped;
			AddDebugLine(out, pointOnCircle(t1, kPi * 0.5f), pointOnCircle(t2, kPi * 0.5f), color);
		}
		// XZ plane
		for (int i = 0; i < clamped; ++i)
		{
			const float p1 = static_cast<float>(i) * kPi / clamped;
			const float p2 = static_cast<float>(i + 1) * kPi / clamped;
			AddDebugLine(out, pointOnCircle(0.0f, p1), pointOnCircle(0.0f, p2), color);
		}
		// YZ plane
		for (int i = 0; i < clamped; ++i)
		{
			const float p1 = static_cast<float>(i) * kPi / clamped;
			const float p2 = static_cast<float>(i + 1) * kPi / clamped;
			AddDebugLine(out, pointOnCircle(kPi * 0.5f, p1), pointOnCircle(kPi * 0.5f, p2), color);
		}
	}

	void AddDebugFrustum(std::vector<DebugVertex>& out, const glm::mat4& viewProj, const glm::vec4& color)
	{
		const glm::mat4 inv = glm::inverse(viewProj);

		// NDC cube corners (clip space)
		const std::array<glm::vec4, 8> ndc{
		        glm::vec4{-1.0f, -1.0f, -1.0f, 1.0f},
		        glm::vec4{1.0f, -1.0f, -1.0f, 1.0f},
		        glm::vec4{1.0f, 1.0f, -1.0f, 1.0f},
		        glm::vec4{-1.0f, 1.0f, -1.0f, 1.0f},
		        glm::vec4{-1.0f, -1.0f, 1.0f, 1.0f},
		        glm::vec4{1.0f, -1.0f, 1.0f, 1.0f},
		        glm::vec4{1.0f, 1.0f, 1.0f, 1.0f},
		        glm::vec4{-1.0f, 1.0f, 1.0f, 1.0f},
		};

		std::array<glm::vec3, 8> world;
		for (std::size_t i = 0; i < ndc.size(); ++i)
		{
			const glm::vec4 h = inv * ndc[i];
			world[i] = glm::vec3(h) / h.w;
		}

		// Near face (z=-1): 0,1,2,3
		AddDebugLine(out, world[0], world[1], color);
		AddDebugLine(out, world[1], world[2], color);
		AddDebugLine(out, world[2], world[3], color);
		AddDebugLine(out, world[3], world[0], color);
		// Far face (z=+1): 4,5,6,7
		AddDebugLine(out, world[4], world[5], color);
		AddDebugLine(out, world[5], world[6], color);
		AddDebugLine(out, world[6], world[7], color);
		AddDebugLine(out, world[7], world[4], color);
		// Connecting edges
		AddDebugLine(out, world[0], world[4], color);
		AddDebugLine(out, world[1], world[5], color);
		AddDebugLine(out, world[2], world[6], color);
		AddDebugLine(out, world[3], world[7], color);
	}

	void AddDebugAxes(std::vector<DebugVertex>& out, const glm::mat4& transform, float length)
	{
		const glm::vec3 origin = glm::vec3(transform[3]);
		const glm::vec3 xAxis = glm::vec3(transform[0]) * length;
		const glm::vec3 yAxis = glm::vec3(transform[1]) * length;
		const glm::vec3 zAxis = glm::vec3(transform[2]) * length;
		AddDebugLine(out, origin, origin + xAxis, glm::vec4(1.0f, 0.2f, 0.2f, 1.0f));
		AddDebugLine(out, origin, origin + yAxis, glm::vec4(0.2f, 1.0f, 0.2f, 1.0f));
		AddDebugLine(out, origin, origin + zAxis, glm::vec4(0.2f, 0.4f, 1.0f, 1.0f));
	}

	void PhysicsDebugRenderer::AppendSelfTestPattern(std::vector<DebugVertex>& out) const
	{
		// DIAGNOSTIC: a fullscreen NDC diamond. Pushed with the bypass flag
		// (tint.w == 2.0). If this is visible the pipeline is alive and writes
		// to the swapchain color; the issue is geometry/transform. If not,
		// the color attachment or pipeline is fundamentally broken.
		constexpr glm::vec4 kMagenta(1.0f, 0.0f, 1.0f, 1.0f);
		AddDebugLine(out, glm::vec3(0.0f, 0.9f, 0.0f), glm::vec3(0.9f, 0.0f, 0.0f), kMagenta);
		AddDebugLine(out, glm::vec3(0.9f, 0.0f, 0.0f), glm::vec3(0.0f, -0.9f, 0.0f), kMagenta);
		AddDebugLine(out, glm::vec3(0.0f, -0.9f, 0.0f), glm::vec3(-0.9f, 0.0f, 0.0f), kMagenta);
		AddDebugLine(out, glm::vec3(-0.9f, 0.0f, 0.0f), glm::vec3(0.0f, 0.9f, 0.0f), kMagenta);
	}

	void PhysicsDebugRenderer::EnsureImmediateBufferCapacity(VmaAllocator allocator, std::uint32_t vertexCount)
	{
		constexpr std::uint32_t kInitialImmediateCapacity = 4096;

		if (m_immediateCapacity >= vertexCount)
		{
			return;
		}

		// Grow geometrically to amortize realloc cost.
		std::uint32_t newCapacity = m_immediateCapacity == 0 ? kInitialImmediateCapacity : m_immediateCapacity;
		while (newCapacity < vertexCount)
		{
			newCapacity *= 2;
		}

		DestroyImmediateBuffer(allocator);

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = static_cast<VkDeviceSize>(newCapacity) * sizeof(DebugVertex),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		const VkResult res = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_immediateVertexBuffer, &m_immediateVertexAlloc, nullptr);
		if (res != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Render, "PhysicsDebugRenderer: failed to allocate immediate vertex buffer ({} verts), error {}", newCapacity, static_cast<int>(res));
			m_immediateCapacity = 0;
			return;
		}
		m_immediateCapacity = newCapacity;
	}

	void PhysicsDebugRenderer::DestroyImmediateBuffer(VmaAllocator allocator)
	{
		if (m_immediateVertexBuffer != VK_NULL_HANDLE)
		{
			vmaDestroyBuffer(allocator, m_immediateVertexBuffer, m_immediateVertexAlloc);
			m_immediateVertexBuffer = VK_NULL_HANDLE;
			m_immediateVertexAlloc = VK_NULL_HANDLE;
		}
	}

	void PhysicsDebugRenderer::RegisterPass(RenderGraph& graph)
	{
		if (!m_enabled)
		{
			return;
		}

		auto color = graph.GetSwapchainColor();
		auto depth = graph.GetSwapchainDepth();

		graph.AddPass("$Debug")
		        .WriteColor(color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .WriteDepth(depth, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_DONT_CARE)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                static int s_frameCount = 0;
			                if (s_frameCount < 5)
			                {
				                AE_INFO(LogCategory::Render,
				                        "[$Debug] frame={} enabled={} pipeline={} frameVertices={} selfTest={} world={}",
				                        s_frameCount,
				                        s_debugRenderingEnabled,
				                        m_pipeline != VK_NULL_HANDLE,
				                        m_frameDebugVertices ? static_cast<int>(m_frameDebugVertices->size()) : -1,
				                        m_selfTestEnabled,
				                        m_world != nullptr);
			                }
			                ++s_frameCount;

			                if (!s_debugRenderingEnabled || m_pipeline == VK_NULL_HANDLE)
			                {
				                return;
			                }

			                CommandRecorder& recorder = ctx.recorder;
			                VkCommandBuffer cmd = recorder.GetCommandBuffer();

			                // Push-constant layout: { uint64 frameAddr, vec4 tint, mat4 model }
			                struct DebugPc
			                {
				                std::uint64_t frameAddr;
				                glm::vec4 tintColor;
				                glm::mat4 model;
			                };
			                static_assert(sizeof(DebugPc) == 88);

			                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

			                // 1) Immediate-mode batched debug primitives.
			                // Combine the per-frame debugVertices (set by the engine from
			                // RenderFramePacket::debugVertices) with the optional self-test
			                // pattern. Both are world-space; we apply identity model and
			                // white tint so per-vertex colors pass through unchanged.
			                std::vector<DebugVertex> scratch;
			                std::vector<DebugVertex>* drawList = nullptr;
			                if (m_frameDebugVertices != nullptr && !m_frameDebugVertices->empty())
			                {
				                drawList = const_cast<std::vector<DebugVertex>*>(m_frameDebugVertices);
			                }
			                if (m_selfTestEnabled)
			                {
				                if (drawList == nullptr)
				                {
					                scratch.reserve(128);
					                drawList = &scratch;
				                }
				                AppendSelfTestPattern(*drawList);
			                }
			                if (drawList != nullptr && !drawList->empty())
			                {
				                const std::uint32_t immediateCount = static_cast<std::uint32_t>(drawList->size());
				                EnsureImmediateBufferCapacity(m_allocator, immediateCount);
				                if (m_immediateVertexBuffer != VK_NULL_HANDLE)
				                {
					                void* mapped = nullptr;
					                vmaMapMemory(m_allocator, m_immediateVertexAlloc, &mapped);
					                std::memcpy(mapped, drawList->data(), static_cast<std::size_t>(immediateCount) * sizeof(DebugVertex));
					                vmaUnmapMemory(m_allocator, m_immediateVertexAlloc);

					                // tint.w = 2.0 -> vertex shader bypasses viewProj and treats positions as NDC.
					                const DebugPc pc{ctx.frameConstantsAddr, glm::vec4(1.0f, 1.0f, 1.0f, 2.0f), glm::mat4(1.0f)};
					                vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

					                constexpr std::uint64_t kBindingOffset = 0;
					                vkCmdBindVertexBuffers(cmd, 0, 1, &m_immediateVertexBuffer, &kBindingOffset);
					                vkCmdDraw(cmd, immediateCount, 1, 0, 0);

					                if (s_frameCount <= 5)
					                {
						                AE_INFO(LogCategory::Render, "[$Debug] drew {} vertices NDC-bypass (frameAddr=0x{:x})", immediateCount, ctx.frameConstantsAddr);
					                }
				                }
				                else
				                {
					                if (s_frameCount <= 5)
					                {
						                AE_INFO(LogCategory::Render, "[$Debug] immediate vertex buffer is NULL");
					                }
				                }
			                }

			                // 2) Physics shape components (boxes/spheres/capsules).
			                // Each entity is one draw with its own model matrix; tint carries the motion-type color
			                // which multiplies into the per-vertex white color baked into the unit geometry.
			                if (m_world != nullptr)
			                {
				                m_world->View<PhysicsDebugShapeComponent, PhysicsStateComponent, RigidBodyComponent>().each(
				                        [&](entt::entity /*entity*/, const PhysicsDebugShapeComponent& shape, const PhysicsStateComponent& state, const RigidBodyComponent& rigid)
				                        {
					                        glm::vec4 tint{1.0f, 1.0f, 0.0f, 1.0f};

					                        if (m_colorMode == PhysicsDebugColorMode::ByMotionType)
					                        {
						                        tint = GetColorForMotionType(rigid.motionType);
					                        }

					                        const glm::mat4 model = glm::translate(glm::mat4(1.0f), state.currPosition) * glm::mat4(state.currRotation) * glm::mat4(glm::scale(glm::mat4(1.0f), state.scale));

					                        VkBuffer vertexBuffer = VK_NULL_HANDLE;
					                        std::uint32_t vertexCount = 0;

					                        switch (shape.shapeType)
					                        {
						                        case PhysicsShapeType::Box:
						                        {
							                        vertexBuffer = m_boxVertexBuffer;
							                        vertexCount = m_boxVertexCount;
							                        break;
						                        }
						                        case PhysicsShapeType::Sphere:
						                        {
							                        vertexBuffer = m_sphereVertexBuffer;
							                        vertexCount = m_sphereVertexCount;
							                        break;
						                        }
						                        case PhysicsShapeType::Capsule:
						                        {
							                        vertexBuffer = m_capsuleVertexBuffer;
							                        vertexCount = m_capsuleVertexCount;
							                        break;
						                        }
					                        }

					                        if (vertexBuffer == VK_NULL_HANDLE || vertexCount == 0)
					                        {
						                        return;
					                        }

					                        const DebugPc pc{ctx.frameConstantsAddr, tint, model};
					                        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

					                        constexpr std::uint64_t kBindingOffset = 0;
					                        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &kBindingOffset);
					                        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
				                        });
			                }
		                });
	}
} // namespace aether
