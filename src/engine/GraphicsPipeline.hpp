#pragma once

#include <string_view>

#include <vulkan/vulkan.h>

namespace meow
{
	class GraphicsPipeline
	{
	public:
		struct Desc
		{
			std::string_view shaderVfsPath;
			std::string_view vertexEntry = "vertexMain";
			std::string_view fragmentEntry = "fragmentMain";
			VkFormat         colorFormat = VK_FORMAT_UNDEFINED;
		};

		GraphicsPipeline() = default;
		~GraphicsPipeline();

		GraphicsPipeline(const GraphicsPipeline&) = delete;
		GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

		GraphicsPipeline(GraphicsPipeline&&) noexcept;
		GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept;

		static GraphicsPipeline Create(VkDevice device, const Desc& desc);
		void Destroy();

		[[nodiscard]] bool             IsValid()    const { return m_pipeline != VK_NULL_HANDLE; }
		[[nodiscard]] VkPipeline       GetPipeline() const { return m_pipeline; }
		[[nodiscard]] VkPipelineLayout GetLayout()   const { return m_layout; }

	private:
		VkDevice         m_device = VK_NULL_HANDLE;
		VkPipelineLayout m_layout = VK_NULL_HANDLE;
		VkPipeline       m_pipeline = VK_NULL_HANDLE;
	};
}
