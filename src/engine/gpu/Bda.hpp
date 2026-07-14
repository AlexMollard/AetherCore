#pragma once

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether::gpu
{
	[[nodiscard]] inline DeviceAddress GetBufferAddress(BufferHandle handle) noexcept
	{
		return ResourceRegistry::ResolveBuffer(handle).deviceAddress;
	}

	[[nodiscard]] inline DeviceAddress GetBufferAddress(BufferHandle handle, DeviceSize byteOffset) noexcept
	{
		return ResourceRegistry::ResolveBuffer(handle).deviceAddress + static_cast<DeviceAddress>(byteOffset);
	}
} // namespace aether::gpu
