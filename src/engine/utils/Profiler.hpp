#pragma once

//                                        (must follow AE_PROFILE_ZONE or AE_PROFILE_ZONE_N

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
