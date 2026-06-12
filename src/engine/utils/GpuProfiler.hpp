#pragma once

// GPU profiling macros (Tracy Vulkan zones).
// When TRACY_ENABLE is on, this header pulls in vulkan/volk.hpp because
// TracyVulkan.hpp has a hard #error guard requiring Vulkan headers to be
// visible first. CPU-only profiling (AE_PROFILE_ZONE / AE_PROFILE_PLOT) lives
// in Profiler.hpp and does not require Vulkan.

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
// Provide a forward-compatible alias for headers that store TracyVkCtx members
// even when Tracy is disabled. The Tracy header itself defines the real type
// when included; this stub keeps translation units that don't include Tracy
// compiling.
using TracyVkCtx = void*;

#	define AE_PROFILE_GPU_ZONE(ctx, cmdbuf, name) (void) 0
#	define AE_PROFILE_GPU_ZONE_T(ctx, cmdbuf, varname, name) (void) 0
#	define AE_PROFILE_GPU_COLLECT(ctx, cmdbuf) (void) 0
#	define AE_PROFILE_GPU_CONTEXT_NAME(ctx, name) (void) 0
#	define AE_PROFILE_GPU_PLOT(name, val) (void) 0
#endif
