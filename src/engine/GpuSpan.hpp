#pragma once

#include <cstdint>
#include "volk.hpp"

namespace aether
{
	// A typed view into a GPU buffer allocation.
	// For device-local (GpuHeap) allocations, data is nullptr - access is GPU-only via address.
	// For host-mapped allocations, data points to the persistently-mapped CPU memory.
	template<typename T>
	struct GpuSpan
	{
		T* data = nullptr;
		VkDeviceAddress address = 0;
		std::uint32_t count = 0;

		[[nodiscard]] bool IsValid() const
		{
			return address != 0;
		}

		[[nodiscard]] VkDeviceAddress DeviceAddress() const
		{
			return address;
		}

		[[nodiscard]] VkDeviceSize ByteSize() const
		{
			return static_cast<VkDeviceSize>(count) * sizeof(T);
		}

		T& operator[](std::size_t i)
		{
			return data[i];
		}

		const T& operator[](std::size_t i) const
		{
			return data[i];
		}

		T* begin()
		{
			return data;
		}

		T* end()
		{
			return data + count;
		}

		const T* begin() const
		{
			return data;
		}

		const T* end() const
		{
			return data + count;
		}
	};
} // namespace aether
