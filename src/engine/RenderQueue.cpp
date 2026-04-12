#include "RenderQueue.hpp"

#include "DrawPushConstants.hpp"
#include "GraphicsPipeline.hpp"
#include "CommandRecorder.hpp"
#include "Mesh.hpp"

namespace meow
{
	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commands.push_back(cmd);
	}

	void RenderQueue::Flush(CommandRecorder& recorder, VkDeviceAddress frameConstantsAddr)
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
				const DrawPushConstants pc{
					.model = cmd.modelMatrix,
					.frameAddr = frameConstantsAddr,
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
