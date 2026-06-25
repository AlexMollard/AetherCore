#pragma once

// Build-derived macros.

// VULKAN_GPU_DEBUG enables GPU-assisted Vulkan validation (GPU-AV + sync
// validation + debug printf + crash diagnostic) in VulkanContext. Off by
// default — GPU-AV injects extra render passes and timestamp queries around
// every draw, which routinely causes TDRs / device-lost on AMD and Intel
// drivers and is expensive enough to mask real performance issues.
//
// Uncomment the line below to opt in for a debugging session.
# define VULKAN_GPU_DEBUG
