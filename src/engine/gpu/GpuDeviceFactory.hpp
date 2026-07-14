#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gpu/GpuTypes.hpp"

// The lifetime contract is "you create it, you destroy it" - there is
namespace aether::gpu::Factory
{

	struct CommandPoolDesc
	{
		std::uint32_t queueFamilyIndex = 0;
		bool transient = false;
		bool resetCommandBuffer = false;
	};

	[[nodiscard]] CommandPool CreateCommandPool(Device device, const CommandPoolDesc& desc) noexcept;
	void DestroyCommandPool(Device device, CommandPool pool) noexcept;

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

	struct SpirvBlob
	{
		const std::byte* data = nullptr;
		std::size_t size = 0;
	};

	[[nodiscard]] Pipeline CreateShaderModule(Device device, const SpirvBlob& spirv, const char* debugName = nullptr) noexcept;
	void DestroyShaderModule(Device device, Pipeline shader) noexcept;

	struct FenceDesc
	{
		bool signaled = false;
	};

	[[nodiscard]] Fence CreateFence(Device device, const FenceDesc& desc) noexcept;
	void DestroyFence(Device device, Fence fence) noexcept;
	[[nodiscard]] bool WaitFence(Device device, Fence fence, std::uint64_t timeoutNs = UINT64_MAX) noexcept;
	void ResetFence(Device device, Fence fence) noexcept;

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

	[[nodiscard]] std::uint32_t GetQueryPoolResults(Device device, QueryPool pool, std::uint32_t firstQuery, std::uint32_t queryCount, std::span<std::uint64_t> outTicks) noexcept;

} // namespace aether::gpu::Factory
