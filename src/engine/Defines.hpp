#pragma once

// they require dev SDKs, add heavy overhead, and must never reach players.
#if defined(AE_CONFIG_DEBUG) || defined(AE_CONFIG_DEV)
#	define AE_DEV_TOOLING 1
#else
#	define AE_DEV_TOOLING 0
#endif

#if AE_DEV_TOOLING
#	define TRACY_ENABLE 1
#	define TRACY_ON_DEMAND 1
#endif

// installed, add heavy CPU/GPU overhead, and must never ship. They are gated on
#if AE_DEV_TOOLING
#	define VULKAN_CPU_DEBUG 1
#	define VULKAN_BEST_PRACTICES 1
#endif
