#include "mesh/MeshUploadQueue.hpp"

#include <cassert>
#include <cstring>

#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	void MeshUploadQueue::Initialize(const VulkanContext& ctx)
	{
		AE_PROFILE_ZONE();
		AE_EXPECT_OR_THROW(buf, UniqueBuffer::CreateMapped(ctx.GetAllocator(), ctx.GetDevice().device, kStagingCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
		m_staging = std::move(buf);
		m_ringHead = 0;
	}

	void MeshUploadQueue::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_staging.Reset();
		m_pendingCopies.clear();
		m_ringHead = 0;
	}

	bool MeshUploadQueue::Upload(const void* vertexData, std::uint64_t vertexBytes, void* destVertexBuffer, std::uint64_t destVertexOffset, const void* indexData, std::uint64_t indexBytes, void* destIndexBuffer, std::uint64_t destIndexOffset)
	{
		AE_PROFILE_ZONE();
		const VkDeviceSize totalBytes = vertexBytes + indexBytes;
		if (m_ringHead + totalBytes > kStagingCapacity)
		{
			return false; // staging full - retry next frame
		}

		auto* mapped = static_cast<std::uint8_t*>(m_staging.GetAllocationInfo().pMappedData);

		std::memcpy(mapped + m_ringHead, vertexData, static_cast<std::size_t>(vertexBytes));
		m_pendingCopies.push_back({static_cast<void*>(m_staging.Get()), m_ringHead, destVertexBuffer, destVertexOffset, vertexBytes});
		m_ringHead += vertexBytes;

		std::memcpy(mapped + m_ringHead, indexData, static_cast<std::size_t>(indexBytes));
		m_pendingCopies.push_back({static_cast<void*>(m_staging.Get()), m_ringHead, destIndexBuffer, destIndexOffset, indexBytes});
		m_ringHead += indexBytes;

		return true;
	}

	void MeshUploadQueue::Flush(gpu::CommandList& cmdList)
	{
		AE_PROFILE_ZONE();
		if (m_pendingCopies.empty())
		{
			return;
		}

		// Flush the host-written staging bytes before the GPU reads them.
		AE_EXPECT_OR_THROW_VOID(m_staging.FlushMapped(0, m_ringHead));

		for (const PendingCopy& copy: m_pendingCopies)
		{
			cmdList.CopyBuffer(copy.srcBuffer, copy.dstBuffer, copy.srcOffset, copy.dstOffset, copy.size);
		}

		// Barrier: transfer-write -> vertex-attribute-read and index-read.
		cmdList.PipelineMemoryBarrier(gpu::PipelineStage::Transfer, gpu::AccessFlags::TransferWrite, gpu::PipelineStage::VertexInput, gpu::AccessFlags::VertexAttributeRead | gpu::AccessFlags::IndexRead);

		m_pendingCopies.clear();
		m_ringHead = 0;
	}
} // namespace aether
