#pragma once

#include <cstdint>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
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

		[[nodiscard]] void* GetCommandPool() const;

	private:
		void* m_impl = nullptr;
	};
} // namespace aether::gpu
