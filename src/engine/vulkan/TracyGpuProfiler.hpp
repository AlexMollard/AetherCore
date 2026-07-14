#pragma once

#include "gpu/GpuProfiler.hpp"

namespace tracy
{
	struct VkCtx;
}

namespace aether::vulkan
{
	// Build a profiler context handle from a Tracy GPU context. The caller owns the handle and must destroy it before the underlying tracy::VkCtx.
	aether::gpu::ProfilerContextHandle CreateTracyGpuProfilerContext(tracy::VkCtx* ctx) noexcept;

	// Destroy a handle. No-op if null. Must be called before the underlying tracy::VkCtx.
	void DestroyTracyGpuProfilerContext(aether::gpu::ProfilerContextHandle handle) noexcept;
} // namespace aether::vulkan
