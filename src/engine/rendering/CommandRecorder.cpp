#include "rendering/CommandRecorder.hpp"

#include "rendering/GraphicsPipeline.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void CommandRecorder::BindGraphicsPipeline(const GraphicsPipeline& pipeline)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::BindPipeline");
		vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.GetPipeline());
	}

	void CommandRecorder::Draw(std::uint32_t vertexCount, std::uint32_t instanceCount, std::uint32_t firstVertex, std::uint32_t firstInstance)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::Draw");
		vkCmdDraw(m_cmd, vertexCount, instanceCount, firstVertex, firstInstance);
	}

	void CommandRecorder::DrawIndirect(VkBuffer indirectBuffer, VkDeviceSize offset, std::uint32_t drawCount, std::uint32_t stride)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::DrawIndirect");
		vkCmdDrawIndirect(m_cmd, indirectBuffer, offset, drawCount, stride);
	}

	void CommandRecorder::DrawIndirectCount(VkBuffer indirectBuffer, VkDeviceSize indirectOffset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxDrawCount, std::uint32_t stride)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::DrawIndirectCount");
		vkCmdDrawIndirectCount(m_cmd, indirectBuffer, indirectOffset, countBuffer, countOffset, maxDrawCount, stride);
	}

	void CommandRecorder::BindIndexBuffer(VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::BindIndexBuffer");
		vkCmdBindIndexBuffer(m_cmd, buffer, offset, indexType);
	}

	void CommandRecorder::BindDescriptorSet(VkPipelineLayout layout, std::uint32_t set, VkDescriptorSet descriptorSet)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::BindDescSet");
		vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, set, 1, &descriptorSet, 0, nullptr);
	}

	void CommandRecorder::PushConstants(VkPipelineLayout layout, const DrawContracts::PushConstants& pc)
	{
		AE_PROFILE_ZONE_N("CmdRecorder::PushConstants");
		vkCmdPushConstants(m_cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawContracts::PushConstants), &pc);
	}

	void CommandRecorder::MemoryBarrier2(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
	{
		if (m_cmd == VK_NULL_HANDLE)
		{
			return;
		}

		const VkMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = srcStage,
			.srcAccessMask = srcAccess,
			.dstStageMask = dstStage,
			.dstAccessMask = dstAccess,
		};
		const VkDependencyInfo dep{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(m_cmd, &dep);
	}

	void CommandRecorder::HostToShaderBarrier()
	{
		MemoryBarrier2(VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	}

	void CommandRecorder::BeginDebugLabel(const char* name, float r, float g, float b, float a)
	{
		if (name == nullptr || m_cmd == VK_NULL_HANDLE || s_beginDebugLabelFn == nullptr)
		{
			return;
		}

		const VkDebugUtilsLabelEXT label{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
			.pLabelName = name,
			.color = { r, g, b, a },
		};
		s_beginDebugLabelFn(m_cmd, &label);
	}

	void CommandRecorder::EndDebugLabel()
	{
		if (m_cmd != VK_NULL_HANDLE && s_endDebugLabelFn != nullptr)
		{
			s_endDebugLabelFn(m_cmd);
		}
	}

	void CommandRecorder::SetDebugLabelFunctions(PFN_vkCmdBeginDebugUtilsLabelEXT beginFn, PFN_vkCmdEndDebugUtilsLabelEXT endFn)
	{
		s_beginDebugLabelFn = beginFn;
		s_endDebugLabelFn = endFn;
	}

	void CommandRecorder::SetObjectNameFunction(PFN_vkSetDebugUtilsObjectNameEXT fn)
	{
		s_setObjectNameFn = fn;
	}

	void CommandRecorder::SetObjectName(VkDevice device, std::uint64_t handle, VkObjectType type, const char* name)
	{
		if (s_setObjectNameFn == nullptr || device == VK_NULL_HANDLE || handle == 0 || name == nullptr)
		{
			return;
		}
		const VkDebugUtilsObjectNameInfoEXT info{
			.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
			.objectType = type,
			.objectHandle = handle,
			.pObjectName = name,
		};
		// Best-effort debug naming - failure is non-critical.
		[[maybe_unused]] const VkResult nameResult = s_setObjectNameFn(device, &info);
	}

	void CommandRecorder::DrawIndexed(std::uint32_t indexCount, std::uint32_t instanceCount, std::uint32_t firstIndex, std::int32_t vertexOffset, std::uint32_t firstInstance)
	{
		vkCmdDrawIndexed(m_cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
	}

	void CommandRecorder::DrawIndexedIndirect(VkBuffer indirectBuffer, VkDeviceSize offset, std::uint32_t drawCount, std::uint32_t stride)
	{
		vkCmdDrawIndexedIndirect(m_cmd, indirectBuffer, offset, drawCount, stride);
	}

	void CommandRecorder::DrawIndexedIndirectCount(VkBuffer indirectBuffer, VkDeviceSize indirectOffset, VkBuffer countBuffer, VkDeviceSize countOffset, std::uint32_t maxDrawCount, std::uint32_t stride)
	{
		vkCmdDrawIndexedIndirectCount(m_cmd, indirectBuffer, indirectOffset, countBuffer, countOffset, maxDrawCount, stride);
	}
} // namespace aether
