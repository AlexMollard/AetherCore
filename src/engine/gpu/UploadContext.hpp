#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	struct ImageUploadRegion
	{
		std::uint64_t offset = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t mipLevel = 0;
		std::int32_t x = 0;
		std::int32_t y = 0;
	};

	class UploadContext
	{
	public:
		UploadContext() = default;
		~UploadContext();

		UploadContext(const UploadContext&) = delete;
		UploadContext& operator=(const UploadContext&) = delete;

		UploadContext(UploadContext&& other) noexcept;
		UploadContext& operator=(UploadContext&& other) noexcept;

		// `transferManager` (a vulkan::TransferManager*) carries CopyBuffer; the graphics
		// queue/pool stay for GetCommandPool's texture uploads.
		[[nodiscard]] static UploadContext Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry, void* transferManager);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_impl != nullptr;
		}

		// Blocking: returns once the copy has completed, so the caller may free `src`.
		void CopyBuffer(BufferHandle src, BufferHandle dst, DeviceSize size);
		// Takes ownership of src until the transfer timeline reaches the returned ticket.
		[[nodiscard]] std::uint64_t CopyBufferAsyncOwned(BufferHandle src, BufferHandle dst, DeviceSize size, DeviceSize destinationOffset = 0, std::function<void()> onStagingRetired = {});
		// Submits on the graphics queue, avoiding cross-family image ownership.
		// Takes ownership of staging on success or failure. Destination must remain
		// alive until the returned ticket completes; zero denotes submission failure.
		[[nodiscard]] std::uint64_t CopyImageAsyncOwned(BufferHandle staging, TextureHandle destination, std::span<const ImageUploadRegion> regions, std::function<void()> onStagingRetired = {});
		// Sequential chunks target a single image. The first transitions all mips
		// to transfer-destination; the final chunk transitions them for sampling.
		[[nodiscard]] std::uint64_t CopyImageChunkAsyncOwned(BufferHandle staging, TextureHandle destination, std::span<const ImageUploadRegion> regions, std::uint32_t mipLevels, bool first, bool last, std::function<void()> onStagingRetired);
		void CollectAsyncUploads();
		[[nodiscard]] bool IsUploadComplete(std::uint64_t ticket) const;
		void WaitForUpload(std::uint64_t ticket);

		[[nodiscard]] void* GetCommandPool() const;

	private:
		void* m_impl = nullptr;
	};
} // namespace aether::gpu
