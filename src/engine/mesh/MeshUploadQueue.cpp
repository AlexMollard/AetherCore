#include "mesh/MeshUploadQueue.hpp"

#include <cassert>
#include <cstring>

#include "utils/Expected.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void MeshUploadQueue::Initialize(const VulkanContext& ctx)
	{
		AE_EXPECT_OR_THROW(buf, UniqueBuffer::CreateMapped(ctx.GetAllocator(), ctx.GetDevice().device, kStagingCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
		m_staging = std::move(*buf);
		m_ringHead = 0;
	}

	void MeshUploadQueue::Shutdown()
	{
		m_staging.Reset();
		m_pendingCopies.clear();
		m_ringHead = 0;
	}

	bool MeshUploadQueue::Upload(const void* vertexData, VkDeviceSize vertexBytes, VkBuffer destVertexBuffer, VkDeviceSize destVertexOffset, const void* indexData, VkDeviceSize indexBytes, VkBuffer destIndexBuffer, VkDeviceSize destIndexOffset)
	{
		const VkDeviceSize totalBytes = vertexBytes + indexBytes;
		if (m_ringHead + totalBytes > kStagingCapacity)
		{
			return false; // staging full - retry next frame
		}

		auto* mapped = static_cast<std::uint8_t*>(m_staging.GetAllocationInfo().pMappedData);

		std::memcpy(mapped + m_ringHead, vertexData, static_cast<std::size_t>(vertexBytes));
		m_pendingCopies.push_back({ m_staging.Get(), m_ringHead, destVertexBuffer, destVertexOffset, vertexBytes });
		m_ringHead += vertexBytes;

		std::memcpy(mapped + m_ringHead, indexData, static_cast<std::size_t>(indexBytes));
		m_pendingCopies.push_back({ m_staging.Get(), m_ringHead, destIndexBuffer, destIndexOffset, indexBytes });
		m_ringHead += indexBytes;

		return true;
	}

	void MeshUploadQueue::Flush(VkCommandBuffer cmd)
	{
		if (m_pendingCopies.empty())
		{
			return;
		}

		// Flush the host-written staging bytes before the GPU reads them.
		vmaFlushAllocation(m_staging.GetAllocator(), m_staging.GetAllocation(), 0, m_ringHead);

		for (const PendingCopy& copy: m_pendingCopies)
		{
			const VkBufferCopy region{
				.srcOffset = copy.srcOffset,
				.dstOffset = copy.dstOffset,
				.size = copy.size,
			};
			vkCmdCopyBuffer(cmd, copy.srcBuffer, copy.dstBuffer, 1, &region);
		}

		// Barrier: transfer-write -> vertex-attribute-read and index-read.
		const VkMemoryBarrier2 barrier{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
			.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT,
		};
		const VkDependencyInfo depInfo{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &barrier,
		};
		vkCmdPipelineBarrier2(cmd, &depInfo);

		m_pendingCopies.clear();
		m_ringHead = 0;
	}
} // namespace aether
