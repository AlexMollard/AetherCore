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
		// device           - opaque VkDevice pointer (gpu::Device)
		// queueFamilyIndex - queue family that will submit the copy
		// queue            - opaque VkQueue that will be used for submits
		// backendRegistry  - opaque pointer to aether::ResourceRegistry
		[[nodiscard]] static UploadContext Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry);

		// Tear down the command pool and release internal state.
		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_impl != nullptr;
		}

		// Record a one-shot vkCmdCopyBuffer between two registry buffers
		// using the stored queue, blocking until completion.
		void CopyBuffer(BufferHandle src, BufferHandle dst, DeviceSize size);

		// Expose internal VkCommandPool for legacy upload code (temporary).
		[[nodiscard]] void* GetCommandPool() const;

	private:
		void* m_impl = nullptr;
	};
} // namespace aether::gpu
