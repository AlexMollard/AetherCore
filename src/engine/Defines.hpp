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

// ── Tracy profiler ──────────────────────────────────────────────────────────
#if defined(AE_CONFIG_DEBUG) || defined(AE_CONFIG_DEV)
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

// ── Vulkan validation ───────────────────────────────────────────────────────
// Uncomment the desired validation level before local development builds.
// GPU-AV is mutually exclusive with VK_EXT_descriptor_heap (see below).
//
// #define VULKAN_CPU_DEBUG 1    // CPU-only validation
// #define VULKAN_GPU_DEBUG 1    // GPU-based validation (slower, more thorough)

// ── Descriptor heap extension ───────────────────────────────────────────────
// Enable VK_EXT_descriptor_heap (Vulkan 1.4). This is incompatible with
// VULKAN_GPU_DEBUG; disable it if you need GPU-AV.
#define AETHERCORE_ENABLE_DESCRIPTOR_HEAP 1
