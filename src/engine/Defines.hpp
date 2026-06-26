#pragma once

// Build-derived macros.

#if defined(VULKAN_CPU_DEBUG) && defined(VULKAN_GPU_DEBUG)
#	error "VULKAN_CPU_DEBUG and VULKAN_GPU_DEBUG are mutually exclusive; pick one"
#endif

// VULKAN_CPU_DEBUG enables the Vulkan validation layer (best-effort
// parameter/object/usage checking, synchronization validation, debug printf)
// and routes all findings through aether::Logger via a debug utils messenger.
// No GPU cost, no render-pass injection, no TDR risk. Safe to leave on in
// any build. Off by default.
//
// VULKAN_GPU_DEBUG additionally enables GPU-Assisted Validation (GPU-AV) and
// the Crash Diagnostic Layer on top of everything VULKAN_CPU_DEBUG enables.
// GPU-AV injects extra render passes and timestamp queries around every draw,
// which routinely causes TDRs / device-lost on AMD and Intel drivers. CDL
// writes a per-queue/per-submit trace to OutputDebugString.
// VULKAN_CPU_DEBUG and VULKAN_GPU_DEBUG are mutually exclusive; setting
// both produces a compile error (see top of file). Off by default.
//
// Uncomment the lines below to opt in for a debugging session.
// # define VULKAN_CPU_DEBUG
# define VULKAN_GPU_DEBUG
