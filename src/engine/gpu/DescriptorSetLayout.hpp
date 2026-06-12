#pragma once

#include <cstdint>
#include <span>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	// Engine-opaque handle to a backend-allocated descriptor set layout.
	// The engine never inspects this; the backend cast lives in
	// vulkan/DescriptorSetLayoutOps.cpp. Defined as a non-owning void*
	// pointer (no destructor on the engine side; the owning subsystem
	// tears it down via DestroyDescriptorSetLayout below).
	using DescriptorSetLayout = void*;

	struct DescriptorSetLayoutDesc
	{
		std::span<const GpuDescriptorSetLayoutBinding> bindings;
		DescriptorSetLayoutFlags flags = DescriptorSetLayoutFlags::None;
	};
} // namespace aether::gpu
