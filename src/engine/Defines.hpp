#pragma once

// ---------------------------------------------------------------------------
// Build-tier configuration for AetherCore.
//
// Exactly one AE_CONFIG_* macro is defined by the build system via CMake
// generator expressions (see src/engine/CMakeLists.txt):
//
//   AE_CONFIG_DEBUG    - Debug build,    full diagnostics, Tracy ON
//   AE_CONFIG_DEV      - Dev build       (RelWithDebInfo), Tracy ON, optimized
//   AE_CONFIG_SHIP     - Shipping build  (Release), Tracy OFF, minimal checks
//   AE_CONFIG_RETAIL   - Retail build    (like Ship but no asserts, LTCG)
//
// Policy decisions (Tracy, assertions, logging) flow from these defines
// so the same binary directory can produce Debug/Dev/Ship/Retail outputs.
//
// Tracy's own library is always compiled with TRACY_ENABLE ON so all profiler
// symbols exist; the linker strips them in Ship/Retail via /OPT:REF /Gy.
// ---------------------------------------------------------------------------

// ── Developer tooling gate ───────────────────────────────────────────────────
// Developer-only, ship-forbidden features (Vulkan validation layers, GPU-AV,
// profilers, etc.) are compiled in ONLY for Debug and Dev (RelWithDebInfo).
// Ship (Release) and Retail (MinSizeRel / retail Release) compile them out:
// they require dev SDKs, add heavy overhead, and must never reach players.
// Gate any such feature on AE_DEV_TOOLING so the policy stays in one place.
#if defined(AE_CONFIG_DEBUG) || defined(AE_CONFIG_DEV)
#	define AE_DEV_TOOLING 1
#else
#	define AE_DEV_TOOLING 0
#endif

// ── Tracy profiler ──────────────────────────────────────────────────────────
#if AE_DEV_TOOLING
#	define TRACY_ENABLE 1
#	define TRACY_ON_DEMAND 1
#endif

// ── Sub-feature toggles (passed as compile definitions from CMake) ───────────
// These are user-configurable at configure time:
//   AETHERCORE_ENABLE_TRACY_GPU     - Vulkan GPU tracing    (default ON)
//   AETHERCORE_ENABLE_TRACY_PLOTS   - Tracy plot/counters   (default ON)
//   AETHERCORE_ENABLE_TRACY_MEMORY  - Tracy memory tracking  (default ON)
// They appear here for documentation; CMake unconditionally defines them
// for all targets so Profiler.hpp / GpuProfiler can check them at runtime.

// ── Vulkan validation (dev tiers only) ──────────────────────────────────────
// Validation layers are developer-only: they need the Vulkan SDK's layers
// installed, add heavy CPU/GPU overhead, and must never ship. They are gated on
// AE_DEV_TOOLING, so Ship/Retail get NONE regardless of the tier chosen below;
// a Release/Retail build always resolves VK_VALIDATION_CPU/GPU to 0 (see the
// #else in VulkanContext.cpp). Within a dev build, pick the tier:
//
//   VULKAN_CPU_DEBUG - core checks + synchronization validation. The everyday
//       tier: near-full API coverage, modest overhead.
//   VULKAN_GPU_DEBUG - GPU-assisted validation + shader debug printf. Deep
//       memory-safety scan (descriptor indexing, BDA, OOB access) that
//       instruments every shader; by design it drops core+sync checks, so it
//       COMPLEMENTS the CPU tier rather than replacing it. ~20s startup while
//       pipelines are instrumented. Empirically verified compatible with
//       VK_EXT_descriptor_heap on SDK 1.4.350 / NVIDIA (proven live via a
//       debugPrintfEXT probe through the debug messenger; a prior note here
//       claiming mutual exclusion was stale). TDR risk on AMD/Intel.
//
#if AE_DEV_TOOLING
#	define VULKAN_CPU_DEBUG 1 // CPU-only validation
//	#define VULKAN_GPU_DEBUG 1 // GPU-assisted deep scan (periodic, not daily)
//
// Best-practices checks (opt-in, composes with either level above). SDK
// 1.4.350's layer has a first-image-use crash in BestPractices::
// ValidateImageInQueue (qf_count integer overflow, bp_image.cpp:280); best-
// practices builds dodge it by dropping the maintenance9 FEATURE BIT (see
// VulkanContext.cpp) -- remove that dodge after an SDK fix.
#	define VULKAN_BEST_PRACTICES 1
#endif

// ── Descriptor heap ──────────────────────────────────────────────────────────
// The renderer requires VK_EXT_descriptor_heap unconditionally (BindlessManager
// has no non-heap path); there is deliberately no toggle for it. GPU-AV works
// with it (see above).
