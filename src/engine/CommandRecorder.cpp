#include "CommandRecorder.hpp"

#include "GraphicsPipeline.hpp"

namespace meow
{
	void CommandRecorder::BindGraphicsPipeline(const GraphicsPipeline& pipeline)
	{
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
	}

	void CommandRecorder::Draw(
		std::uint32_t vertexCount,
		std::uint32_t instanceCount,
		std::uint32_t firstVertex,
		std::uint32_t firstInstance)
	{
		vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void CommandRecorder::BindVertexBuffer(VkBuffer buffer, VkDeviceSize offset)
	{
		vkCmdBindVertexBuffers(m_cmd, 0, 1, &buffer, &offset);
	}

	void CommandRecorder::BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
	{
		vkCmdBindIndexBuffer(m_cmd, buffer, offset, indexType);
	}

	void CommandRecorder::BindDescriptorSet(VkPipelineLayout layout, std::uint32_t set, VkDescriptorSet descriptorSet)
	{
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set, 1, &descriptorSet, 0, nullptr);
	}

	void CommandRecorder::PushConstants(VkPipelineLayout layout, const DrawPushConstants& pc)
	{
		vkCmdPushConstants(m_cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(DrawPushConstants), &pc);
	}

	void CommandRecorder::DrawIndexed(
		std::uint32_t indexCount,
		std::uint32_t instanceCount,
		std::uint32_t firstIndex,
		std::int32_t  vertexOffset,
		std::uint32_t firstInstance)
	{
		vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}
}
