#include "RenderQueue.hpp"

#include "CommandRecorder.hpp"
#include "Mesh.hpp"

namespace meow
{
	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commands.push_back(cmd);
	}

	void RenderQueue::Flush(CommandRecorder& recorder)
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
			}

			if (cmd.mesh != nullptr)
			{
				recorder.BindVertexBuffer(cmd.mesh->GetBuffer());
			}

			const std::uint32_t count = (cmd.mesh != nullptr)
				? cmd.mesh->GetVertexCount()
				: cmd.vertexCount;

			recorder.Draw(count, cmd.instanceCount);
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
