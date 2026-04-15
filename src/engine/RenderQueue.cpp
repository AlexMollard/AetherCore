#include "RenderQueue.hpp"

#include <algorithm>
#include <format>

#include "CommandRecorder.hpp"
#include "DrawPushConstants.hpp"
#include "GraphicsPipeline.hpp"
#include "Mesh.hpp"

namespace aether
{
	void RenderQueue::Submit(const DrawCommand& cmd)
	{
		m_commands.push_back(cmd);
	}

	void RenderQueue::Flush(CommandRecorder& recorder, VkDeviceAddress frameConstantsAddr, VkDescriptorSet bindlessSet, VkDescriptorSet lightingSet)
	{
		if (!recorder.IsValid())
		{
			return;
		}

		const GraphicsPipeline* lastPipeline = nullptr;
		const Mesh* lastMesh = nullptr;
		VkBuffer lastVertexBuffer = VK_NULL_HANDLE;
		VkBuffer lastIndexBuffer = VK_NULL_HANDLE;
		const GraphicsPipeline* lastSetPipeline = nullptr;
		const GraphicsPipeline* lastLightingSetPipeline = nullptr;

		std::stable_sort(m_commands.begin(),
		        m_commands.end(),
		        [](const DrawCommand& a, const DrawCommand& b)
		        {
			        if (a.pipeline != b.pipeline)
			        {
				        return a.pipeline < b.pipeline;
			        }
			        if (a.mesh != b.mesh)
			        {
				        return a.mesh < b.mesh;
			        }
			        return a.materialIndex < b.materialIndex;
		        });

		recorder.BeginDebugLabel("RenderQueue.Flush", 0.85f, 0.60f, 0.18f, 1.0f);

		for (const DrawCommand& cmd: m_commands)
		{
			const std::string drawLabel = std::format("Draw P={} M={} Mat={}{}", reinterpret_cast<const void*>(cmd.pipeline), reinterpret_cast<const void*>(cmd.mesh), cmd.materialIndex, (cmd.mesh != nullptr && cmd.mesh->IsIndexed()) ? " Indexed" : " NonIndexed");
			recorder.BeginDebugLabel(drawLabel.c_str(), 0.95f, 0.40f, 0.25f, 1.0f);
			if (cmd.pipeline != nullptr)
			{
				if (cmd.pipeline != lastPipeline)
				{
					recorder.BindGraphicsPipeline(*cmd.pipeline);
					lastPipeline = cmd.pipeline;
					lastLightingSetPipeline = nullptr;
				}

				// Bind the bindless descriptor set (set 0) if supplied.
				if (bindlessSet != VK_NULL_HANDLE && cmd.pipeline == lastPipeline)
				{
					// Descriptor set 0 is tied to pipeline layout; rebind on pipeline
					// switch.
					if (cmd.pipeline != lastSetPipeline)
					{
						vkCmdBindDescriptorSets(recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, cmd.pipeline->GetLayout(), 0, 1, &bindlessSet, 0, nullptr);
						lastSetPipeline = cmd.pipeline;
					}
				}

				// Bind optional set 1 (tiled/clustered lighting) when supported.
				if (lightingSet != VK_NULL_HANDLE && cmd.pipeline == lastPipeline && cmd.pipeline->GetSetLayoutCount() > 1)
				{
					if (cmd.pipeline != lastLightingSetPipeline)
					{
						vkCmdBindDescriptorSets(recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS, cmd.pipeline->GetLayout(), 1, 1, &lightingSet, 0, nullptr);
						lastLightingSetPipeline = cmd.pipeline;
					}
				}

				const DrawPushConstants pc{
					.model = cmd.modelMatrix,
					.frameAddr = frameConstantsAddr,
					.materialIndex = cmd.materialIndex,
					.skinBufferAddr = cmd.skinBufferAddr,
				};
				recorder.PushConstants(cmd.pipeline->GetLayout(), pc);
			}

			if (cmd.mesh != nullptr)
			{
				if (cmd.mesh != lastMesh || cmd.mesh->GetBuffer() != lastVertexBuffer)
				{
					recorder.BindVertexBuffer(cmd.mesh->GetBuffer());
					lastVertexBuffer = cmd.mesh->GetBuffer();
				}

				if (cmd.mesh->IsIndexed())
				{
					if (cmd.mesh != lastMesh || cmd.mesh->GetIndexBuffer() != lastIndexBuffer)
					{
						recorder.BindIndexBuffer(cmd.mesh->GetIndexBuffer());
						lastIndexBuffer = cmd.mesh->GetIndexBuffer();
					}
				}

				lastMesh = cmd.mesh;
			}

			if (cmd.mesh != nullptr && cmd.mesh->IsIndexed())
			{
				recorder.DrawIndexed(cmd.mesh->GetIndexCount(), cmd.instanceCount);
			}
			else
			{
				const std::uint32_t count = (cmd.mesh != nullptr) ? cmd.mesh->GetVertexCount() : cmd.vertexCount;

				recorder.Draw(count, cmd.instanceCount);
			}

			recorder.EndDebugLabel();
		}

		recorder.EndDebugLabel();
	}

	void RenderQueue::Clear()
	{
		m_commands.clear();
	}

	bool RenderQueue::IsEmpty() const
	{
		return m_commands.empty();
	}
} // namespace aether
