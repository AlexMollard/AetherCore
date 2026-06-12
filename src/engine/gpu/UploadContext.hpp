#pragma once

#include <cstdint>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// RAII helper for one-shot GPU buffer-to-buffer copies.
	//
	// Manages a single VkCommandPool and records transient command buffers
	// via gpu::OneShotCmd.  All Vulkan details are hidden in the backend
	// (vulkan/UploadContext.cpp); this header is Vulkan-free.
	class UploadContext
	{
	public:
		UploadContext() = default;
		~UploadContext();

		UploadContext(const UploadContext&) = delete;
		UploadContext& operator=(const UploadContext&) = delete;

		UploadContext(UploadContext&& other) noexcept;
		UploadContext& operator=(UploadContext&& other) noexcept;

		// Allocate a command pool on the given queue family.
		// device           — opaque VkDevice pointer (gpu::Device)
		// queueFamilyIndex — queue family that will submit the copy
		// backendRegistry  — opaque pointer to aether::ResourceRegistry
		[[nodiscard]] static UploadContext Create(Device device, std::uint32_t queueFamilyIndex, void* backendRegistry);

		// Tear down the command pool and release internal state.
		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_impl != nullptr;
		}

		// Record a one-shot vkCmdCopyBuffer between two registry buffers
		// and submit it to the provided queue, blocking until completion.
		void CopyBuffer(Queue queue, BufferHandle src, BufferHandle dst, DeviceSize size);

	private:
		void* m_impl = nullptr;
	};
} // namespace aether::gpu
