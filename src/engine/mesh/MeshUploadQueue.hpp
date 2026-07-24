#pragma once

#include <cstdint>
#include <vector>

#include "gpu/CommandList.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether::vulkan
{
	class TransferManager;
}

namespace aether
{
	class MeshUploadQueue
	{
	public:
		static constexpr std::uint64_t kStagingCapacity = 64ull * 1024 * 1024;

		void Initialize();
		void Shutdown();

		bool Upload(const void* vertexData, std::uint64_t vertexBytes, gpu::Buffer destVertexBuffer, std::uint64_t destVertexOffset, const void* indexData, std::uint64_t indexBytes, gpu::Buffer destIndexBuffer, std::uint64_t destIndexOffset);

		// Submit the pending copies to the transfer queue and reset the staging ring.
		// Non-blocking: ordering against rendering comes from the frame submission
		// waiting on the transfer timeline (GpuDevice::SubmitAndPresent), and the
		// staging ring self-guards - the first Upload of the next cycle waits for the
		// previous flush's ticket before overwriting staged bytes (a frame later, that
		// is virtually always already complete).
		std::uint64_t FlushAsync(vulkan::TransferManager& transfer);

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

		void RecordCopies(gpu::CommandList& cmdList);

		gpu::BufferHandle m_stagingHandle{};
		void* m_stagingMapped = nullptr;
		gpu::Buffer m_stagingBuffer = nullptr;
		std::uint64_t m_ringHead = 0;
		std::vector<PendingCopy> m_pendingCopies;
		vulkan::TransferManager* m_transfer = nullptr;
		std::uint64_t m_lastFlushTicket = 0;
	};
} // namespace aether
