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

	static bool s_debugRenderingEnabled = false;

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
	        m_colorMode(rhs.m_colorMode),
	        m_world(rhs.m_world),
	        m_viewProj(rhs.m_viewProj),
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
	        m_capsuleVertexCount(rhs.m_capsuleVertexCount)
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
	}

	PhysicsDebugRenderer& PhysicsDebugRenderer::operator=(PhysicsDebugRenderer&& rhs) noexcept
	{
		if (this != &rhs)
		{
			Shutdown(rhs.m_device);
			m_device = rhs.m_device;
			m_allocator = rhs.m_allocator;
			m_enabled = rhs.m_enabled;
			m_colorMode = rhs.m_colorMode;
			m_world = rhs.m_world;
			m_viewProj = rhs.m_viewProj;
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

			rhs.m_device = VK_NULL_HANDLE;
			rhs.m_pipeline = VK_NULL_HANDLE;
			rhs.m_pipelineLayout = VK_NULL_HANDLE;
			rhs.m_boxVertexBuffer = VK_NULL_HANDLE;
			rhs.m_boxVertexAlloc = VK_NULL_HANDLE;
			rhs.m_sphereVertexBuffer = VK_NULL_HANDLE;
			rhs.m_sphereVertexAlloc = VK_NULL_HANDLE;
			rhs.m_capsuleVertexBuffer = VK_NULL_HANDLE;
			rhs.m_capsuleVertexAlloc = VK_NULL_HANDLE;
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
		        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		        .offset = 0,
		        .size = 80,
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

		const VkVertexInputBindingDescription kBinding{
		        .binding = 0,
		        .stride = sizeof(glm::vec3),
		        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		const VkVertexInputAttributeDescription kAttrib{
		        .location = 0,
		        .binding = 0,
		        .format = VK_FORMAT_R32G32B32_SFLOAT,
		        .offset = 0,
		};

		const VkPipelineVertexInputStateCreateInfo vertexInput{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		        .vertexBindingDescriptionCount = 1,
		        .pVertexBindingDescriptions = &kBinding,
		        .vertexAttributeDescriptionCount = 1,
		        .pVertexAttributeDescriptions = &kAttrib,
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
		        .depthTestEnable = VK_TRUE,
		        .depthWriteEnable = VK_TRUE,
		        .depthCompareOp = VK_COMPARE_OP_LESS,
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

		m_boxVertexCount = static_cast<std::uint32_t>(kBoxEdges.size());

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = sizeof(kBoxEdges),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_boxVertexBuffer, &m_boxVertexAlloc, nullptr);

		void* data = nullptr;
		vmaMapMemory(allocator, m_boxVertexAlloc, &data);
		std::memcpy(data, kBoxEdges.data(), sizeof(kBoxEdges));
		vmaUnmapMemory(allocator, m_boxVertexAlloc);
	}

	void PhysicsDebugRenderer::CreateSphereGeometry(VmaAllocator allocator)
	{
		std::vector<glm::vec3> vertices;
		vertices.reserve(64 * 3);

		constexpr int kSegments = 16;
		constexpr float kRingRadius = 0.5f;
		constexpr float kTubeRadius = 0.5f;
		constexpr float kTwoPi = 6.28318530718f;
		constexpr float kPi = 3.14159265359f;

		for (int i = 0; i < kSegments; ++i)
		{
			const float theta1 = static_cast<float>(i * kTwoPi) / kSegments;
			const float theta2 = static_cast<float>((i + 1) * kTwoPi) / kSegments;

			for (int j = 0; j < kSegments; ++j)
			{
				const float phi1 = static_cast<float>(j * kPi) / kSegments;
				const float phi2 = static_cast<float>((j + 1) * kPi) / kSegments;

				vertices.push_back(glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta1), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta1)});
				vertices.push_back(glm::vec3{kTubeRadius * std::sin(phi1) * std::cos(theta2), kTubeRadius * std::cos(phi1), kTubeRadius * std::sin(phi1) * std::sin(theta2)});

				vertices.push_back(glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta1), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta1)});
				vertices.push_back(glm::vec3{kTubeRadius * std::sin(phi2) * std::cos(theta2), kTubeRadius * std::cos(phi2), kTubeRadius * std::sin(phi2) * std::sin(theta2)});
			}
		}

		m_sphereVertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = vertices.size() * sizeof(glm::vec3),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_sphereVertexBuffer, &m_sphereVertexAlloc, nullptr);

		void* data = nullptr;
		vmaMapMemory(allocator, m_sphereVertexAlloc, &data);
		std::memcpy(data, vertices.data(), vertices.size() * sizeof(glm::vec3));
		vmaUnmapMemory(allocator, m_sphereVertexAlloc);
	}

	void PhysicsDebugRenderer::CreateCapsuleGeometry(VmaAllocator allocator)
	{
		std::vector<glm::vec3> vertices;

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

				vertices.push_back(glm::vec3{kRadius * std::sin(phi1) * std::cos(theta1), kHalfHeight + kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta1)});
				vertices.push_back(glm::vec3{kRadius * std::sin(phi1) * std::cos(theta2), kHalfHeight + kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta2)});

				vertices.push_back(glm::vec3{kRadius * std::sin(phi2) * std::cos(theta1), kHalfHeight + kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta1)});
				vertices.push_back(glm::vec3{kRadius * std::sin(phi2) * std::cos(theta2), kHalfHeight + kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta2)});

				vertices.push_back(glm::vec3{kRadius * std::sin(phi1) * std::cos(theta1), -kHalfHeight - kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta1)});
				vertices.push_back(glm::vec3{kRadius * std::sin(phi1) * std::cos(theta2), -kHalfHeight - kRadius * std::cos(phi1), kRadius * std::sin(phi1) * std::sin(theta2)});

				vertices.push_back(glm::vec3{kRadius * std::sin(phi2) * std::cos(theta1), -kHalfHeight - kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta1)});
				vertices.push_back(glm::vec3{kRadius * std::sin(phi2) * std::cos(theta2), -kHalfHeight - kRadius * std::cos(phi2), kRadius * std::sin(phi2) * std::sin(theta2)});
			}
		}

		m_capsuleVertexCount = static_cast<std::uint32_t>(vertices.size());

		const VkBufferCreateInfo bufferInfo{
		        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		        .size = vertices.size() * sizeof(glm::vec3),
		        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
		};

		VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_CPU_TO_GPU,
		};

		vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &m_capsuleVertexBuffer, &m_capsuleVertexAlloc, nullptr);

		void* data = nullptr;
		vmaMapMemory(allocator, m_capsuleVertexAlloc, &data);
		std::memcpy(data, vertices.data(), vertices.size() * sizeof(glm::vec3));
		vmaUnmapMemory(allocator, m_capsuleVertexAlloc);
	}

	void PhysicsDebugRenderer::RegisterPass(RenderGraph& graph)
	{
		if (!m_enabled)
		{
			return;
		}

		auto color = graph.GetSwapchainColor();
		auto depth = graph.GetSwapchainDepth();

		graph.AddPass("$PhysicsDebug")
		        .WriteColor(color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .ReadTexture(depth)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (!s_debugRenderingEnabled || !m_world || m_pipeline == VK_NULL_HANDLE)
			                {
				                return;
			                }

			                CommandRecorder& recorder = ctx.recorder;
			                VkCommandBuffer cmd = recorder.GetCommandBuffer();

			                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

			                m_world->View<PhysicsDebugShapeComponent, PhysicsStateComponent, RigidBodyComponent>().each(
			                        [&](entt::entity entity, const PhysicsDebugShapeComponent& shape, const PhysicsStateComponent& state, const RigidBodyComponent& rigid)
			                        {
				                        glm::vec4 color{1.0f, 1.0f, 0.0f, 1.0f};

				                        if (m_colorMode == PhysicsDebugColorMode::ByMotionType)
				                        {
					                        color = GetColorForMotionType(rigid.motionType);
				                        }

				                        glm::mat4 model = glm::translate(glm::mat4(1.0f), state.currPosition) * glm::mat4(state.currRotation);
				                        glm::mat4 scaledModel = model * glm::mat4(glm::scale(glm::mat4(1.0f), state.scale));
				                        glm::mat4 mvp = m_viewProj * scaledModel;

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

				                        constexpr std::uint64_t kBindingOffset = 0;
				                        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &kBindingOffset);

				                        alignas(16) float pushData[20];
				                        std::memcpy(pushData, &mvp, sizeof(mvp));
				                        pushData[16] = color.r;
				                        pushData[17] = color.g;
				                        pushData[18] = color.b;
				                        pushData[19] = color.a;
				                        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), pushData);

				                        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
			                        });
		                });
	}
} // namespace aether
