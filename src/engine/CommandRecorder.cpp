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
}
