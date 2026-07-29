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

// Whether the Vulkan validation layer is on when nobody asks for anything.
//
// It is compiled into both dev configs (VULKAN_CPU_DEBUG above), but being compiled in
// and being on by default are different questions. The layer is the dominant CPU cost in
// the editor - measured at 115 fps with it against 288 fps without on the same scene, and
// it degrades further over a long session as it accumulates per-frame state - while
// costing nothing in VRAM. So:
//
//   Debug          - on. This is the safety net that catches resource-lifetime and image
//                    layout errors during render work, and it stays on.
//   RelWithDebInfo - off. This is the config the editor is actually used in, where the
//                    frame rate matters and the layer is pure overhead.
//   Release/Retail - not compiled in at all; a shipped game never sees any of this.
//
// Either default can be overridden per run: --validation turns it on, --no-validation
// turns it off. See ResolveValidationEnabled in ProjectCli.hpp for the precedence.
#if defined(AE_CONFIG_DEBUG)
#	define AE_VALIDATION_DEFAULT_ON 1
#else
#	define AE_VALIDATION_DEFAULT_ON 0
#endif

// Human-readable build tier, for log lines that have to say which set of defaults is in
// force. Kept next to the flags it describes so the two cannot drift apart.
#if defined(AE_CONFIG_DEBUG)
#	define AE_CONFIG_NAME "Debug"
#elif defined(AE_CONFIG_DEV)
#	define AE_CONFIG_NAME "RelWithDebInfo"
#elif defined(AE_CONFIG_RETAIL)
#	define AE_CONFIG_NAME "Retail"
#elif defined(AE_CONFIG_SHIP)
#	define AE_CONFIG_NAME "Release"
#else
#	define AE_CONFIG_NAME "Unknown"
#endif
