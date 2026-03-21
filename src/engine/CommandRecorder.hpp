#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace meow
{
	class GraphicsPipeline;

	class CommandRecorder
	{
	public:
		CommandRecorder() = default;
		explicit CommandRecorder(VkCommandBuffer cmd) : m_cmd(cmd) {}

		[[nodiscard]] bool IsValid() const { return m_cmd != VK_NULL_HANDLE; }

		void BindGraphicsPipeline(const GraphicsPipeline& pipeline);
		void Draw(
			std::uint32_t vertexCount,
			std::uint32_t instanceCount = 1,
			std::uint32_t firstVertex = 0,
			std::uint32_t firstInstance = 0);
		void BindVertexBuffer(VkBuffer buffer, VkDeviceSize offset = 0);

		// For engine-internal use only (Swapchain, MeowCore, etc.)
		[[nodiscard]] VkCommandBuffer GetRaw() const { return m_cmd; }

	private:
		VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	};
}
