#include "mesh/MeshUploadQueue.hpp"

#include <cassert>
#include <cstring>

#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/TransferManager.hpp"

namespace aether
{
	void MeshUploadQueue::Initialize()
	{
		AE_PROFILE_ZONE();

		const gpu::MappedBufferDesc desc{
		        .size = kStagingCapacity,
		        .usage = gpu::BufferUsage::TransferSrc,
		        .memoryUsage = gpu::MappedMemoryUsage::Auto,
		        .debugName = "MeshUploadQueue.Staging",
		};
		m_stagingHandle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!m_stagingHandle.IsValid())
		{
			Throw(AetherError::Engine("MeshUploadQueue: CreateMappedBuffer failed"));
		}
		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_stagingHandle);
		m_stagingMapped = view.mappedPtr;
		m_stagingBuffer = static_cast<gpu::Buffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(m_stagingHandle));
		m_ringHead = 0;
	}

	void MeshUploadQueue::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (m_stagingHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_stagingHandle);
			m_stagingHandle = {};
		}
		m_stagingMapped = nullptr;
		m_stagingBuffer = nullptr;
		m_pendingCopies.clear();
		m_ringHead = 0;
	}

	bool MeshUploadQueue::Upload(
	        const void* vertexData, std::uint64_t vertexBytes, gpu::Buffer destVertexBuffer, std::uint64_t destVertexOffset, const void* indexData, std::uint64_t indexBytes, gpu::Buffer destIndexBuffer, std::uint64_t destIndexOffset)
	{
		AE_PROFILE_ZONE();
		const std::uint64_t totalBytes = vertexBytes + indexBytes;
		if (m_ringHead + totalBytes > kStagingCapacity)
		{
			return false;
		}

		// First write of a new cycle: the previous flush's copies may still be reading
		// the staging ring on the transfer queue - never overwrite until they are done.
		// A frame later this ticket is virtually always complete, so this is a no-op in
		// steady state, not a stall.
		if (m_ringHead == 0 && m_lastFlushTicket != 0 && m_transfer != nullptr)
		{
			m_transfer->WaitFor(m_lastFlushTicket);
			m_lastFlushTicket = 0;
		}

		auto* mapped = static_cast<std::uint8_t*>(m_stagingMapped);

		std::memcpy(mapped + m_ringHead, vertexData, static_cast<std::size_t>(vertexBytes));
		m_pendingCopies.push_back({.srcBuffer = m_stagingBuffer, .srcOffset = m_ringHead, .dstBuffer = destVertexBuffer, .dstOffset = destVertexOffset, .size = vertexBytes});
		m_ringHead += vertexBytes;

		std::memcpy(mapped + m_ringHead, indexData, static_cast<std::size_t>(indexBytes));
		m_pendingCopies.push_back({.srcBuffer = m_stagingBuffer, .srcOffset = m_ringHead, .dstBuffer = destIndexBuffer, .dstOffset = destIndexOffset, .size = indexBytes});
		m_ringHead += indexBytes;

		return true;
	}

	std::uint64_t MeshUploadQueue::FlushAsync(vulkan::TransferManager& transfer)
	{
		AE_PROFILE_ZONE();
		if (m_pendingCopies.empty())
		{
			return 0;
		}

		m_transfer = &transfer;
		m_lastFlushTicket = transfer.Submit([this](gpu::CommandList& cmdList) { RecordCopies(cmdList); });
		return m_lastFlushTicket;
	}

	// No barrier after the copies: consuming stages (vertex input) are not legal on a
	// transfer-only queue, and the frame submission's wait on the transfer timeline
	// already orders the copies before any rendering and makes them visible.
	void MeshUploadQueue::RecordCopies(gpu::CommandList& cmdList)
	{
		gpu::ResourceRegistry::FlushMappedBuffer(m_stagingHandle, 0, m_ringHead);

		for (const PendingCopy& copy: m_pendingCopies)
		{
			cmdList.CopyBuffer(copy.srcBuffer, copy.dstBuffer, copy.srcOffset, copy.dstOffset, copy.size);
		}

		m_pendingCopies.clear();
		m_ringHead = 0;
	}
} // namespace aether
