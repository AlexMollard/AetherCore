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
	// Build a profiler context handle from a Tracy GPU context. The
	// caller owns the returned handle and must destroy it with
	// `DestroyTracyGpuProfilerContext` before destroying the
	// `tracy::VkCtx`. Returns `nullptr` if TRACY_ENABLE is off.
	aether::gpu::ProfilerContextHandle CreateTracyGpuProfilerContext(tracy::VkCtx* ctx) noexcept;

	// Destroy a handle returned by `CreateTracyGpuProfilerContext`.
	// No-op if `handle` is null. Must be called before the
	// underlying `tracy::VkCtx` is destroyed.
	void DestroyTracyGpuProfilerContext(aether::gpu::ProfilerContextHandle handle) noexcept;
} // namespace aether::vulkan
