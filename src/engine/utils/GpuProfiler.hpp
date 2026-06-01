#pragma once

// GPU profiling macros (Tracy Vulkan zones).
// Include this ONLY from translation units that already pull in Vulkan headers,
// or let this header pull volk in for you - it includes volk.hpp when TRACY_ENABLE
// is set so TracyVulkan.hpp has the Vulkan symbols it requires.
// For CPU-only profiling use Profiler.hpp instead.

#ifdef TRACY_ENABLE
#	include <cstring>
#	include "vulkan/volk.hpp"
#	include <tracy/TracyVulkan.hpp>

#	define AE_PROFILE_GPU_ZONE(ctx, cmdbuf, name) TracyVkZone(ctx, cmdbuf, name)
#	define AE_PROFILE_GPU_ZONE_T(ctx, cmdbuf, varname, name) TracyVkZoneTransient(ctx, varname, cmdbuf, name, true)
#	define AE_PROFILE_GPU_COLLECT(ctx, cmdbuf) TracyVkCollect(ctx, cmdbuf)
#	define AE_PROFILE_GPU_CONTEXT_NAME(ctx, name) TracyVkContextName(ctx, name, static_cast<std::uint16_t>(std::strlen(name)))

// GPU-variant plot: records a value on the GPU timeline in Tracy.
// Falls back to TracyPlot on the CPU timeline when the GPU context isn't available.
#	define AE_PROFILE_GPU_PLOT(name, val) TracyPlot(name, val)
#else
// Provide the type even when Tracy is disabled so headers that store TracyVkCtx
// members compile without pulling in any Vulkan headers.
using TracyVkCtx = void*;

#	define AE_PROFILE_GPU_ZONE(ctx, cmdbuf, name) (void) 0
#	define AE_PROFILE_GPU_ZONE_T(ctx, cmdbuf, varname, name) (void) 0
#	define AE_PROFILE_GPU_COLLECT(ctx, cmdbuf) (void) 0
#	define AE_PROFILE_GPU_CONTEXT_NAME(ctx, name) (void) 0
#	define AE_PROFILE_GPU_PLOT(name, val) (void) 0
#endif
