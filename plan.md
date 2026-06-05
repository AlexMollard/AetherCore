# Vulkan 1.4 Audit Fix Plan

## Phase 1 — Barrier Spec Fixes & Pipeline Cache

### 1.1 Fix depth barrier src stages with UNDEFINED layout
**File:** `src/engine/vulkan/Swapchain.cpp:317-325`
**Issue:** `VK_IMAGE_LAYOUT_UNDEFINED` used with `EARLY/LATE_FRAGMENT_TESTS` src stages — spec violation.
**Fix:** Use `VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT` + `VK_ACCESS_2_NONE` as src.
Add frame-to-frame synchronization via fence/stored layout tracking so sync validation
doesn't flag the single shared depth image as a write-after-write hazard.
**Risk:** Low. RenderGraph already manages layout states; the swapchain depth image just
needs its last-known layout preserved across frames.

### 1.2 Add VkPipelineCache
**Files:** `src/engine/vulkan/VulkanContext.hpp`, `src/engine/vulkan/VulkanContext.cpp`, all pipeline creation sites
**Issue:** All pipelines created with `VK_NULL_HANDLE` pipeline cache.
**Fix:** Create a single `VkPipelineCache` at device init, store in `VulkanContext`, pass to:
- `GraphicsPipeline::Create` (`src/engine/rendering/GraphicsPipeline.cpp:195`)
- `RenderQueueSharedPipelines::Initialize` (`src/engine/rendering/RenderQueue.cpp:995+`)
- `LightingManager::EnsureComputePipeline` (`src/engine/rendering/LightingManager.cpp:554+`)
- `CullPass`, `LocalShadowService`, `QuadRenderer`, any remaining `vkCreateComputePipelines` calls
Optionally serialize/deserialize from disk for faster warm starts.
**Risk:** Low. Pipeline cache is transparent; fallback on cache miss is automatic.

### 1.3 Fix vague queue handle casting
**File:** `src/engine/vulkan/VulkanContext.cpp:231-239`
**Issue:** `reinterpret_cast<std::uint64_t>(m_graphicsQueue)` where `VkQueue` may not be a pointer.
**Fix:** Use `reinterpret_cast<std::uint64_t>(static_cast<void*>(m_graphicsQueue))` for pointer-safe cast.
**Risk:** Trivial.

---

## Phase 2 — Memory Safety & Upload Path Sanitation

### 2.1 Deferred buffer destruction in LightingManager
**File:** `src/engine/rendering/LightingManager.cpp:479-505`
**Issue:** `ensureBuffer` destroys old buffer immediately; may be in-flight on GPU.
**Fix:** Move old buffer to a per-frame deferred-destruction queue, destroyed after
`kMaxFramesInFlight` frames (like RenderGraph does for transient images).
**Risk:** Medium. Requires adding a deferred-destruction ring buffer.

### 2.2 Replace vkQueueWaitIdle in GpuHeap uploads
**File:** `src/engine/vulkan/GpuHeap.cpp:107-173`
**Issue:** `UploadBytes()` calls `vkQueueWaitIdle` — full GPU drain per upload.
**Fix:** Use a persistent staging ring-buffer with per-frame fence completion tracking.
Submit uploads to the graphics/transfer queue without waiting; advance ring on next
use of the same slot once the fence is signaled. If GpuHeap is truly only used during
initialization, add a `#ifndef NDEBUG` assertion that it's not called after the first frame.
**Risk:** Medium. Requires a ring-buffer allocator or accepting that GpuHeap is init-only.

### 2.3 Replace vkQueueWaitIdle in texture upload paths
**File:** `src/engine/material/Texture.cpp:55-87`
**Issue:** `EndAndSubmitOneTimeBuffer` calls `vkQueueWaitIdle` after every upload.
**Fix:** Same as 2.2 — use fence-based synchronization. Since texture uploads already
use `vkCopyMemoryToImageEXT` (hostImageCopy), the command buffer only does a
layout transition. Consider batching all upload layout transitions into one submission.
**Risk:** Medium.

---

## Phase 3 — API Correctness & Cleanup

### 3.1 Fix PushConstants ignoring pipeline layout ranges
**File:** `src/engine/rendering/CommandRecorder.hpp:41`, `CommandRecorder.cpp:44-48`
**Issue:** `PushConstants()` always pushes `sizeof(DrawContracts::PushConstants)` with
`VERTEX|FRAGMENT` stages regardless of the bound layout's actual push constant ranges.
**Fix:** Either (a) template on push constant type, or (b) add a `PushConstantsRaw()` 
overload that takes explicit stageFlags + size, and rename the current one to 
`PushDrawConstants()`. Callers using compute pipelines must use the raw variant.
**Risk:** Medium. Touches the CommandRecorder public API.

### 3.2 Add push constant size validation at startup
**File:** `src/engine/vulkan/VulkanContext.cpp`
**Issue:** No check that `VkPhysicalDeviceLimits::maxPushConstantsSize >= sizeof(largest pc struct)`.
**Fix:** After physical device selection, read `maxPushConstantsSize` and assert it meets
the minimum required (128 bytes guaranteed by 1.0, but verify against actual struct sizes).
**Risk:** Trivial.

### 3.3 Remove redundant Vulkan 1.1 feature enables
**File:** `src/engine/vulkan/VulkanContext.cpp:99-100`
**Issue:** `shaderDrawParameters` is core in Vulkan 1.1 — redundant with 1.4 requirement.
**Fix:** Remove from `VkPhysicalDeviceVulkan11Features` or add a comment noting it's
left for SDK compatibility with pre-1.4 validation.
**Risk:** Trivial.

---

## Phase 4 — Modernization (Future Work)

### 4.1 VK_KHR_push_descriptor
Eliminates per-frame descriptor pool management for frequently-updated descriptor sets
(lighting, material constants). High impact, moderate effort.

### 4.2 VK_EXT_graphics_pipeline_library
Faster pipeline creation for material variants. Good for a content-heavy engine.
Moderate effort.

### 4.3 VK_EXT_mesh_shader
Replace traditional vertex/index pipeline with mesh shaders for GPU-driven rendering.
Large effort, future consideration.

### 4.4 VK_KHR_fragment_shading_rate
Variable rate shading for performance on supported hardware.
Moderate effort.

---

## Implementation Order
```
Phase 1 → Phase 2 → Phase 3 → Phase 4
  (start here)
```
Each phase should be verified with sync validation enabled (GPU-AV + sync validation)
before moving to the next phase.
