#pragma once

#include <cstdint>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether
{
	class MeshUploadQueue
	{
	public:
		static constexpr std::uint64_t kStagingCapacity = 64ull * 1024 * 1024;

		void Initialize();
		void Shutdown();

		bool Upload(const void* vertexData, std::uint64_t vertexBytes, gpu::Buffer destVertexBuffer, std::uint64_t destVertexOffset, const void* indexData, std::uint64_t indexBytes, gpu::Buffer destIndexBuffer, std::uint64_t destIndexOffset);

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
