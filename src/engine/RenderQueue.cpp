#include "RenderQueue.hpp"

#include "DrawPushConstants.hpp"
#include "GraphicsPipeline.hpp"
#include "CommandRecorder.hpp"
#include "Mesh.hpp"

namespace aether
{
	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commands.push_back(cmd);
	}

	void RenderQueue::Flush(
		CommandRecorder& recorder,
		VkDeviceAddress  frameConstantsAddr,
		VkDescriptorSet  bindlessSet)
	{
		if (!recorder.IsValid())
		{
			return;
		}

		for (const DrawCommand& cmd : m_commands)
		{
			if (cmd.pipeline != nullptr)
			{
				recorder.BindGraphicsPipeline(*cmd.pipeline);

				// Bind the bindless descriptor set (set 0) if supplied.
				if (bindlessSet != VK_NULL_HANDLE)
				{
					vkCmdBindDescriptorSets(
						recorder.GetCommandBuffer(),
						VK_PIPELINE_BIND_POINT_GRAPHICS,
						cmd.pipeline->GetLayout(),
						0, 1, &bindlessSet,
						0, nullptr);
				}

				const DrawPushConstants pc{
					.model         = cmd.modelMatrix,
					.frameAddr     = frameConstantsAddr,
					.materialIndex = cmd.materialIndex,
				};
				recorder.PushConstants(cmd.pipeline->GetLayout(), pc);
			}

			if (cmd.mesh != nullptr)
			{
				recorder.BindVertexBuffer(cmd.mesh->GetBuffer());

				if (cmd.mesh->IsIndexed())
				{
					recorder.BindIndexBuffer(cmd.mesh->GetIndexBuffer());
				}
			}

			if (cmd.mesh != nullptr && cmd.mesh->IsIndexed())
			{
				recorder.DrawIndexed(cmd.mesh->GetIndexCount(), cmd.instanceCount);
			}
			else
			{
				const std::uint32_t count = (cmd.mesh != nullptr)
					? cmd.mesh->GetVertexCount()
					: cmd.vertexCount;

				recorder.Draw(count, cmd.instanceCount);
			}
		}
	}

	void RenderQueue::Clear()
	{
		m_commands.clear();
	}

	bool RenderQueue::IsEmpty() const
	{
		return m_commands.empty();
	}
}
