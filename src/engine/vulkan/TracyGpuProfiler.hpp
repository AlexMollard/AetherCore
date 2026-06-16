#pragma once

// Vulkan-side factory for the GPU profiler pImpl.
//
// This is the ONLY way to build a `aether::gpu::ProfilerContextHandle`
// from a `tracy::VkCtx*`. The handle is the engine-side pImpl that
// hides Tracy's types. Lives in vulkan/ because it is the only place
// that names `tracy::VkCtx*`.
//
// Usage from `vulkan/VulkanContext.cpp`:
//
//   auto* handle = aether::vulkan::CreateTracyGpuProfilerContext(m_tracyVkCtx);
//   aether::gpu::GpuProfiler::Get().Initialize({ handle });
//   ...
//   aether::vulkan::DestroyTracyGpuProfilerContext(handle);

#include "gpu/GpuProfiler.hpp"

namespace tracy
{
	class VkCtx;
} // namespace tracy

namespace aether::vulkan
{
	// Build a profiler context handle from a Tracy GPU context. The caller owns the handle and must destroy it before the underlying tracy::VkCtx.
	aether::gpu::ProfilerContextHandle CreateTracyGpuProfilerContext(tracy::VkCtx* ctx) noexcept;

	// Destroy a handle. No-op if null. Must be called before the underlying tracy::VkCtx.
	void DestroyTracyGpuProfilerContext(aether::gpu::ProfilerContextHandle handle) noexcept;
} // namespace aether::vulkan
