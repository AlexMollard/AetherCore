#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "DrawPushConstants.hpp"

namespace meow
{
	class GraphicsPipeline;

	class CommandRecorder
	{
	public:
		CommandRecorder() = default;
		explicit CommandRecorder(VkCommandBuffer cmd) : m_cmd(cmd) {}

		[[nodiscard]] bool IsValid() const { return m_cmd != VK_NULL_HANDLE; }
		[[nodiscard]] VkCommandBuffer GetCommandBuffer() const { return m_cmd; }

		void BindGraphicsPipeline(const GraphicsPipeline& pipeline);
		void Draw(
			std::uint32_t vertexCount,
			std::uint32_t instanceCount = 1,
			std::uint32_t firstVertex = 0,
			std::uint32_t firstInstance = 0);
		void DrawIndexed(
			std::uint32_t indexCount,
			std::uint32_t instanceCount = 1,
			std::uint32_t firstIndex = 0,
			std::int32_t  vertexOffset = 0,
			std::uint32_t firstInstance = 0);
		void BindVertexBuffer(VkBuffer buffer, VkDeviceSize offset = 0);
		void BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset = 0, VkIndexType indexType = VK_INDEX_TYPE_UINT32);
		void BindDescriptorSet(VkPipelineLayout layout, std::uint32_t set, VkDescriptorSet descriptorSet);
		void PushConstants(VkPipelineLayout layout, const DrawPushConstants& pc);
	private:
		VkCommandBuffer m_cmd = VK_NULL_HANDLE;
	};
}
