#pragma once

#include <cstdint>
#include "gpu/GpuTypes.hpp"
#include "utils/Assert.hpp"
#include "vulkan/volk.hpp"

namespace aether
{
	template<typename T>
	struct GpuSpan
	{
		T* data = nullptr;
		gpu::DeviceAddress address = 0;
		std::uint32_t count = 0;

		[[nodiscard]] bool IsValid() const
		{
			return address != 0;
		}

		[[nodiscard]] gpu::DeviceAddress DeviceAddress() const
		{
			return address;
		}

		[[nodiscard]] VkDeviceSize ByteSize() const
		{
			return static_cast<VkDeviceSize>(count) * sizeof(T);
		}

		T& operator[](std::size_t i)
		{
			AE_ASSERT(data != nullptr, "Cannot index device-local GpuSpan from CPU");
			AE_ASSERT(i < count, "GpuSpan index out of bounds");
			return data[i];
		}

		const T& operator[](std::size_t i) const
		{
			AE_ASSERT(data != nullptr, "Cannot index device-local GpuSpan from CPU");
			AE_ASSERT(i < count, "GpuSpan index out of bounds");
			return data[i];
		}

		T* begin()
		{
			AE_ASSERT(data != nullptr, "Cannot iterate device-local GpuSpan from CPU");
			return data;
		}

		T* end()
		{
			AE_ASSERT(data != nullptr, "Cannot iterate device-local GpuSpan from CPU");
			return data + count;
		}

		[[nodiscard]] const T* begin() const
		{
			AE_ASSERT(data != nullptr, "Cannot iterate device-local GpuSpan from CPU");
			return data;
		}

		[[nodiscard]] const T* end() const
		{
			AE_ASSERT(data != nullptr, "Cannot iterate device-local GpuSpan from CPU");
			return data + count;
		}
	};
} // namespace aether
