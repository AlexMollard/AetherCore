#pragma once

// Buffer-device-address (BDA) host helpers.
//
// Every BDA access in the engine goes through gpu::ResourceRegistry::ResolveBuffer(h)
// followed by .deviceAddress. These shorthands remove the boilerplate and give a
// single named call site for address reads, including the common suballocation case
// (base address + byte offset).
//
// The helpers are zero-overhead one-liners over ResolveBuffer; they exist for
// ergonomics and to make the BDA access pattern greppable.

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/ResourceRegistry.hpp"

namespace aether::gpu
{
	// Returns the device address of the buffer backing `handle`, or 0 if stale.
	[[nodiscard]] inline DeviceAddress GetBufferAddress(BufferHandle handle) noexcept
	{
		return ResourceRegistry::ResolveBuffer(handle).deviceAddress;
	}

	// Returns the device address of `handle` offset by `byteOffset` bytes.
	// Useful for suballocations carved out of a larger buffer (GpuHeap spans,
	// render-graph transient slices, per-frame-slot regions).
	[[nodiscard]] inline DeviceAddress GetBufferAddress(BufferHandle handle, DeviceSize byteOffset) noexcept
	{
		return ResourceRegistry::ResolveBuffer(handle).deviceAddress + static_cast<DeviceAddress>(byteOffset);
	}
}
