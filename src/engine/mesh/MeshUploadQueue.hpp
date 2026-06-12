#pragma once

#include <cstdint>
#include <vector>

#include "gpu/CommandList.hpp"
#include "vulkan/UniqueBuffer.hpp"

namespace aether
{
	class VulkanContext;

	// Batches pending dynamic mesh vertex/index uploads into a persistently-mapped
	// staging ring buffer, then emits all vkCmdCopyBuffer calls and a single
	// transfer -> vertex/index barrier when Flush() is called.
	//
	// The ring resets after every Flush, so the effective budget per frame is
	// kStagingCapacity bytes.  If Upload() returns false the mesh is deferred to
	// the next frame (the arena allocation is already committed; only the GPU-side
	// data is missing until the next successful flush).
	//
	// Usage per frame:
	//   1. Call Upload(...) for each dirty chunk that produced new geometry.
	//   2. Call Flush(cmd) once - before the cull compute pass runs.
	class MeshUploadQueue
	{
	public:
		// 64 MB staging budget per frame.  Increase for higher chunk-spawn rates.
		static constexpr std::uint64_t kStagingCapacity = 64ull * 1024 * 1024;

		void Initialize(const VulkanContext& ctx);
		void Shutdown();

		// Stage vertexBytes + indexBytes and enqueue copy commands to the arena
		// destination buffers.
		// Returns false if the staging ring would overflow - caller should retry
		// next frame.
		bool Upload(const void* vertexData, std::uint64_t vertexBytes, void* destVertexBuffer, std::uint64_t destVertexOffset, const void* indexData, std::uint64_t indexBytes, void* destIndexBuffer, std::uint64_t destIndexOffset);

		// Record all pending copy commands into cmd, then insert a
		// transfer-write -> vertex-input/index-read barrier.
		// Call this once per frame before the cull compute pass.
		void Flush(gpu::CommandList& cmdList);

		[[nodiscard]] bool HasPendingUploads() const
		{
			return !m_pendingCopies.empty();
		}

	private:
		struct PendingCopy
		{
			void* srcBuffer;
			std::uint64_t srcOffset;
			void* dstBuffer;
			std::uint64_t dstOffset;
			std::uint64_t size;
		};

		UniqueBuffer m_staging;
		std::uint64_t m_ringHead = 0;
		std::vector<PendingCopy> m_pendingCopies;
	};
} // namespace aether
