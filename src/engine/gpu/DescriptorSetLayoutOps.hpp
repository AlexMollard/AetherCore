#pragma once

#include "gpu/DescriptorSetLayout.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class GpuDevice;
}

namespace aether::gpu
{
	// Backend helpers for creating / destroying a descriptor set layout.
	// Engine code calls these directly to avoid leaking Vk* into
	// the engine seam. The current implementation is Vulkan; a future
	// D3D12 / Metal backend would provide its own .cpp.
	[[nodiscard]] Expected<DescriptorSetLayout> CreateDescriptorSetLayout(GpuDevice& device, const DescriptorSetLayoutDesc& desc) noexcept;
	void DestroyDescriptorSetLayout(GpuDevice& device, DescriptorSetLayout layout) noexcept;
} // namespace aether::gpu
