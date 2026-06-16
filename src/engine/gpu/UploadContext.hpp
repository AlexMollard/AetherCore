#pragma once

#include <cstdint>

#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// RAII helper for one-shot GPU buffer-to-buffer copies. Backend owns
	// the pool + command buffer; the engine just calls CopyBuffer.
	class UploadContext
	{
	public:
		UploadContext() = default;
		~UploadContext();

		UploadContext(const UploadContext&) = delete;
		UploadContext& operator=(const UploadContext&) = delete;

		UploadContext(UploadContext&& other) noexcept;
		UploadContext& operator=(UploadContext&& other) noexcept;

		// backendRegistry is the opaque aether::ResourceRegistry pointer.
		[[nodiscard]] static UploadContext Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_impl != nullptr;
		}

		// One-shot copy src -> dst, blocking until completion.
		void CopyBuffer(BufferHandle src, BufferHandle dst, DeviceSize size);

		// Opaque handle to the backend command pool.
		[[nodiscard]] void* GetCommandPool() const;

	private:
		void* m_impl = nullptr;
	};
} // namespace aether::gpu
