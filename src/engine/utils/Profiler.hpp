#pragma once

// ---------------------------------------------------------------------------
// AetherCore profiling macros - thin wrappers around Tracy.
//
// Enable at configure time:  cmake -DAETHERCORE_ENABLE_TRACY=ON ...
//
// CPU zones:
//   AE_PROFILE_ZONE()                  - auto-named zone (function + file + line)
//   AE_PROFILE_ZONE_N("name")          - compile-time named zone
//   AE_PROFILE_SET_ZONE_NAME(cstr)     - override zone name with a runtime string
//                                        (must follow AE_PROFILE_ZONE or AE_PROFILE_ZONE_N
//                                        in the same scope)
//   AE_PROFILE_FRAME                   - mark the end of a rendered frame
//
// Thread naming:
//   AE_PROFILE_THREAD("IOThread")      - name the calling thread in the profiler
//
// Memory tracking (CPU heap):
//   AE_PROFILE_ALLOC(ptr, size)        - report an allocation to Tracy
//   AE_PROFILE_FREE(ptr)               - report a deallocation to Tracy
//
// Named memory pools (e.g. "GPU"):
//   AE_PROFILE_ALLOC_N(ptr, size, pool)
//   AE_PROFILE_FREE_N(ptr, pool)
//
// Plots:
//   AE_PROFILE_PLOT(name, value)
//   AE_PROFILE_PLOT_CONFIG(name, type, step, fill, color)
// ---------------------------------------------------------------------------

#ifndef AETHERCORE_ENABLE_TRACY_PLOTS
#	define AETHERCORE_ENABLE_TRACY_PLOTS 1
#endif

#ifndef AETHERCORE_ENABLE_TRACY_MEMORY
#	define AETHERCORE_ENABLE_TRACY_MEMORY 1
#endif

#ifdef TRACY_ENABLE
#	include <cstring>
#	include <tracy/Tracy.hpp>

#	define AE_PROFILE_ZONE() ZoneScoped
#	define AE_PROFILE_ZONE_N(name) ZoneScopedN(name)
#	define AE_PROFILE_SET_ZONE_NAME(cstr) ZoneName(cstr, std::strlen(cstr))
#	define AE_PROFILE_FRAME FrameMark
#	define AE_PROFILE_THREAD(name) tracy::SetThreadName(name)
#	if AETHERCORE_ENABLE_TRACY_MEMORY
#		define AE_PROFILE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#		define AE_PROFILE_FREE(ptr) TracyFree(ptr)
#		define AE_PROFILE_ALLOC_N(ptr, size, pool) TracyAllocN(ptr, size, pool)
#		define AE_PROFILE_FREE_N(ptr, pool) TracyFreeN(ptr, pool)
#	else
#		define AE_PROFILE_ALLOC(ptr, size) (void) 0
#		define AE_PROFILE_FREE(ptr) (void) 0
#		define AE_PROFILE_ALLOC_N(ptr, size, pool) (void) 0
#		define AE_PROFILE_FREE_N(ptr, pool) (void) 0
#	endif
#	if AETHERCORE_ENABLE_TRACY_PLOTS
#		define AE_PROFILE_PLOT(name, val) TracyPlot(name, val)
#		define AE_PROFILE_PLOT_CONFIG(name, type, step, fill, color) TracyPlotConfig(name, type, step, fill, color)
#	else
#		define AE_PROFILE_PLOT(name, val) (void) 0
#		define AE_PROFILE_PLOT_CONFIG(name, type, step, fill, color) (void) 0
#	endif
#else
#	define AE_PROFILE_ZONE() (void) 0
#	define AE_PROFILE_ZONE_N(name) (void) 0
#	define AE_PROFILE_SET_ZONE_NAME(cstr) (void) 0
#	define AE_PROFILE_FRAME (void) 0
#	define AE_PROFILE_THREAD(name) (void) 0
#	define AE_PROFILE_ALLOC(ptr, size) (void) 0
#	define AE_PROFILE_FREE(ptr) (void) 0
#	define AE_PROFILE_ALLOC_N(ptr, size, pool) (void) 0
#	define AE_PROFILE_FREE_N(ptr, pool) (void) 0
#	define AE_PROFILE_PLOT(name, val) (void) 0
#	define AE_PROFILE_PLOT_CONFIG(name, type, step, fill, color) (void) 0
#endif
