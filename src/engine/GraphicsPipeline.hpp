#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

namespace aether
{
	class GraphicsPipeline
	{
	public:
		struct Desc
		{
			std::string_view shaderVfsPath;
			std::string_view vertexEntry = "vertexMain";
			std::string_view fragmentEntry = "fragmentMain";
			VkFormat colorFormat = VK_FORMAT_UNDEFINED;
			VkFormat depthFormat = VK_FORMAT_UNDEFINED;
			bool depthTestEnable = false;
			bool depthWriteEnable = false;
			VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS; // Enable standard src-alpha / one-minus-src-alpha
			                                                 // blending on color attachment 0.
			bool blendEnable = false;                        // Set true for full-screen / procedural passes
			                                                 // that generate vertices
			// from SV_VertexID — no vertex buffer or attribute declarations needed.
			bool noVertexInput = false;
			// Use a reduced mesh layout (POSITION + JOINTS_0 + WEIGHTS_0 only).
			// Intended for depth-only shadow caster pipelines.
			bool shadowVertexInput = false;
			// Override the default DrawPushConstants block. If size is 0 the
			// default model-matrix + BDA range is used instead.
			uint32_t pushConstantSize = 0;
			VkShaderStageFlags pushConstantStages = VK_SHADER_STAGE_ALL_GRAPHICS;
			// Descriptor set layouts bound into the pipeline layout in order.
			std::span<const VkDescriptorSetLayout> setLayouts;
		};

		GraphicsPipeline() = default;
		~GraphicsPipeline();

		GraphicsPipeline(const GraphicsPipeline&) = delete;
		GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

		GraphicsPipeline(GraphicsPipeline&&) noexcept;
		GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept;

		static GraphicsPipeline Create(VkDevice device, const Desc& desc);
		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_pipeline != VK_NULL_HANDLE;
		}

		[[nodiscard]] VkPipeline GetPipeline() const
		{
			return m_pipeline;
		}

		[[nodiscard]] VkPipelineLayout GetLayout() const
		{
			return m_layout;
		}

		[[nodiscard]] std::uint32_t GetSetLayoutCount() const
		{
			return m_setLayoutCount;
		}

	private:
		VkDevice m_device = VK_NULL_HANDLE;
		VkPipelineLayout m_layout = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;
		std::uint32_t m_setLayoutCount = 0;
	};
} // namespace aether
