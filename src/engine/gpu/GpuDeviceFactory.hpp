#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gpu/GpuTypes.hpp"

// Engine-side factories for transient GPU resources that don't fit
// the ResourceRegistry model (no long-lived handle, no bindless slot,
// no deferred-destruction ring). Examples: command pools, query pools,
// shader modules, fences, binary semaphores.
//
// Every factory takes a `gpu::Device` (opaque VkDevice pointer) as
// its first argument and returns `gpu::` opaque types. The actual
// VkCreate* / VkDestroy* calls live in vulkan/GpuDeviceFactory.cpp.
//
// The lifetime contract is "you create it, you destroy it" - there is
// no automatic tracking. Sites that need pooled / registry-managed
// lifetime should use gpu::ResourceRegistry::Create* instead.
//
// Note: samplers are NOT created through this factory. Engine code
// should use BindlessManager::GetOrCreateSampler(Filter,
// SamplerMipmapMode, SamplerAddressMode), which returns
// Expected<gpu::Sampler> and caches by key. Timeline semaphores use
// gpu::CreateTimelineSemaphore (see gpu/Semaphore.hpp).
namespace aether::gpu::Factory
{
	// -----------------------------------------------------------------
	// CommandPool
	// -----------------------------------------------------------------

	struct CommandPoolDesc
	{
		std::uint32_t queueFamilyIndex = 0;
		bool transient = false;          // VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
		bool resetCommandBuffer = false; // VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
	};

	[[nodiscard]] CommandPool CreateCommandPool(Device device, const CommandPoolDesc& desc) noexcept;
	void DestroyCommandPool(Device device, CommandPool pool) noexcept;

	// -----------------------------------------------------------------
	// QueryPool
	// -----------------------------------------------------------------

	enum class QueryType : std::uint32_t
	{
		Timestamp = 0,
		Occlusion = 1,
		PipelineStatistics = 2,
	};

	struct QueryPoolDesc
	{
		QueryType type = QueryType::Timestamp;
		std::uint32_t count = 0;
	};

	[[nodiscard]] QueryPool CreateQueryPool(Device device, const QueryPoolDesc& desc) noexcept;
	void DestroyQueryPool(Device device, QueryPool pool) noexcept;
	void ResetQueryPool(Device device, QueryPool pool, std::uint32_t firstQuery, std::uint32_t queryCount) noexcept;

	// -----------------------------------------------------------------
	// ShaderModule
	// -----------------------------------------------------------------

	// Owns a SPIR-V blob. The factory wraps the bytes in a
	// VkShaderModuleCreateInfo and returns the resulting opaque
	// handle. Engine code should keep the SpirvBlob alive for as
	// long as the shader module is in use (the backend borrows the
	// bytes at create time only).
	struct SpirvBlob
	{
		const std::byte* data = nullptr;
		std::size_t size = 0;
	};

	[[nodiscard]] Pipeline CreateShaderModule(Device device, const SpirvBlob& spirv, const char* debugName = nullptr) noexcept;
	void DestroyShaderModule(Device device, Pipeline shader) noexcept;

	// -----------------------------------------------------------------
	// Fence
	// -----------------------------------------------------------------

	struct FenceDesc
	{
		bool signaled = false; // VK_FENCE_CREATE_SIGNALED_BIT
	};

	[[nodiscard]] Fence CreateFence(Device device, const FenceDesc& desc) noexcept;
	void DestroyFence(Device device, Fence fence) noexcept;
	[[nodiscard]] bool WaitFence(Device device, Fence fence, std::uint64_t timeoutNs = UINT64_MAX) noexcept;
	void ResetFence(Device device, Fence fence) noexcept;

	// Note: semaphores are NOT created through this factory. Engine
	// code should use gpu::CreateTimelineSemaphore (gpu/Semaphore.hpp),
	// which returns a TimelineSemaphore* with wait / destroy helpers.
	// The Vulkan 1.2+ recommendation is one timeline semaphore per
	// queue per frame, not N binary semaphores. Binary semaphores are
	// only used in vulkan/Swapchain.cpp because the WSI layer
	// (VkPresentInfoKHR) requires binary semaphores for present-wait
	// on platforms that don't support
	// VK_SWAPCHAIN_CREATE_PRESENT_TIMELINE_BIT. Sites that need
	// cross-frame sync (AsyncComputeContext, RenderGraphStorage) all use the timeline factory instead.

	// -----------------------------------------------------------------
	// PhysicalDevice queries
	// -----------------------------------------------------------------
	// Engine-facing mirror of the subset of VkPhysicalDeviceProperties
	// the engine currently consumes. Extend with new fields as needed
	// (mirroring the VkPhysicalDeviceProperties layout one-for-one).

	struct PhysicalDeviceLimits
	{
		bool timestampComputeAndGraphics = false;
		float timestampPeriod = 1.0f;
	};

	struct PhysicalDeviceProperties
	{
		PhysicalDeviceLimits limits{};
	};

	[[nodiscard]] PhysicalDeviceProperties GetPhysicalDeviceProperties(PhysicalDevice physicalDevice) noexcept;

	// -----------------------------------------------------------------
	// QueryPool result readback
	// -----------------------------------------------------------------
	// Read `queryCount` 64-bit timestamp values from a query pool.
	// Returns the number of values actually written into `outTicks`
	// (0 on failure). Mirrors vkGetQueryPoolResults with the
	// VK_QUERY_RESULT_64_BIT flag.
	[[nodiscard]] std::uint32_t GetQueryPoolResults(Device device, QueryPool pool, std::uint32_t firstQuery, std::uint32_t queryCount, std::span<std::uint64_t> outTicks) noexcept;

} // namespace aether::gpu::Factory
