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

		[[nodiscard]] static UploadContext Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry);

		void Destroy();

		[[nodiscard]] bool IsValid() const
		{
			return m_impl != nullptr;
		}

		void CopyBuffer(BufferHandle src, BufferHandle dst, DeviceSize size);

		[[nodiscard]] void* GetCommandPool() const;

	private:
		void* m_impl = nullptr;
	};
} // namespace aether::gpu
