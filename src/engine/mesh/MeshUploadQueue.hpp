#pragma once

#include <cstdint>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether
{
	// Batches pending dynamic-mesh vertex/index uploads into a persistently
	// mapped staging ring, then emits all buffer-copy calls and a single
	// transfer -> vertex/index barrier in Flush().
	class MeshUploadQueue
	{
	public:
		// 64 MB staging budget per frame.
		static constexpr std::uint64_t kStagingCapacity = 64ull * 1024 * 1024;

		void Initialize();
		void Shutdown();

		// Stage vertexBytes + indexBytes and enqueue copies into the arena
		// destination buffers. Returns false if the ring would overflow -
		// the caller should retry next frame.
		bool Upload(const void* vertexData, std::uint64_t vertexBytes, gpu::Buffer destVertexBuffer, std::uint64_t destVertexOffset, const void* indexData, std::uint64_t indexBytes, gpu::Buffer destIndexBuffer, std::uint64_t destIndexOffset);

		// Record all pending copy commands into cmd, then insert a
		// transfer-write -> vertex-input/index-read barrier. Call once per
		// frame before the cull compute pass.
		void Flush(gpu::CommandList& cmdList);

		[[nodiscard]] bool HasPendingUploads() const
		{
			return !m_pendingCopies.empty();
		}

	private:
		struct PendingCopy
		{
			gpu::Buffer srcBuffer = nullptr;
			std::uint64_t srcOffset = 0;
			gpu::Buffer dstBuffer = nullptr;
			std::uint64_t dstOffset = 0;
			std::uint64_t size = 0;
		};

		gpu::BufferHandle m_stagingHandle{};
		void* m_stagingMapped = nullptr;
		gpu::Buffer m_stagingBuffer = nullptr;
		std::uint64_t m_ringHead = 0;
		std::vector<PendingCopy> m_pendingCopies;
	};
} // namespace aether
