#include "mesh/MeshUploadQueue.hpp"

#include <cassert>
#include <cstring>

#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"

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
			return false; // staging full - retry next frame
		}

		auto mapped = static_cast<std::uint8_t*>(m_stagingMapped);

		std::memcpy(mapped + m_ringHead, vertexData, static_cast<std::size_t>(vertexBytes));
		m_pendingCopies.push_back({.srcBuffer = m_stagingBuffer, .srcOffset = m_ringHead, .dstBuffer = destVertexBuffer, .dstOffset = destVertexOffset, .size = vertexBytes});
		m_ringHead += vertexBytes;

		std::memcpy(mapped + m_ringHead, indexData, static_cast<std::size_t>(indexBytes));
		m_pendingCopies.push_back({.srcBuffer = m_stagingBuffer, .srcOffset = m_ringHead, .dstBuffer = destIndexBuffer, .dstOffset = destIndexOffset, .size = indexBytes});
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
		gpu::ResourceRegistry::FlushMappedBuffer(m_stagingHandle, 0, m_ringHead);

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
