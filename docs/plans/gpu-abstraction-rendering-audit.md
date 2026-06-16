# GPU Abstraction & Rendering Audit

> **Audience:** an LLM coding agent (or human) consuming this audit to prioritize the
> remaining GPU/rendering cleanup work.
> **Repo:** AetherCore. C++26 (Clang) / C++23 (MSVC). Vulkan 1.3, volk, VMA, vk-bootstrap.
> **Scope:** `src/engine/gpu/`, `src/engine/vulkan/`, `src/engine/rendering/`,
> `src/engine/passes/`, plus the engine-side owners in `material/`, `mesh/`, `text/`,
> `assets/`, `animation/`, `ui/`, `physics/`.
> **Date:** 2026-06-15.

This audit replaces the older phased plan
(`docs/plans/gpu-abstraction-rendering-refactor.md`, deleted) which has drifted from
the actual codebase. The current state is captured below; remaining work is
prioritized at the end.

---

## 1. Current State Summary

The engine has a **two-layer GPU abstraction**, but it is enforced unevenly.
A clean `gpu/` facade exists for the hot draw path (CommandList, handles, enums),
but several engine-side subsystems still drag `volk.hpp` / `vk_mem_alloc.h` and
call raw `vk*` for resource creation, upload synchronisation, query pools, and
sampler/descriptor-set-layout creation.

### What's clean

- **`gpu::CommandList`** is a real Vulkan-free facade. Header is Vulkan-free
  (`src/engine/gpu/CommandList.hpp`); impl casts `void*` to `Vk*` in
  `CommandList.cpp` only. Bind/Draw/Dispatch/PushConstants/Barriers/Debug labels
  all route through the facade.
- **`gpu::Format`** is a complete enum with bidirectional
  `gpu::ToVk` / `gpu::FromVk` in `vulkan/GpuEnumConversions.cpp`. No
  engine code declares `VkFormat` directly any more (the residual sites are
  internal to `vulkan/`).
- **Typed handles** (`TextureHandle`, `BufferHandle`, `PipelineHandle`,
  `SamplerHandle` in `gpu/GpuHandles.hpp`) with generation counters and
  `IsValid()`. `ResourceRegistry` resolves them to `Vk*` at the seam.
- **`FrameContext`** carries the per-frame wiring; `BindRuntime` / `RegisterPass`
  no longer take 8-11 arg lists (`RenderTargetService.cpp:46-53` shows the
  `AE_ASSERT` guards).
- **`RenderGraph.hpp` is Vulkan-free.** Internals are kept Vulkan-side via
  a pImpl `RenderGraphStorage` (`vulkan/RenderGraphStorage.hpp` + `.cpp`).
  `Execute()` consumes `gpu::CommandList&` and dispatches through the
  storage; barrier solver, transient heap, async-compute resources, image
  cache, and event pool all live in the backend TU.
- **App layer is clean.** `rg "volk.hpp|<vulkan/|static_cast<Vk|\bCommandRecorder\b"
  src/app` returns nothing. The `CommandRecorder` files are fully deleted.
- **No mutable global singletons.** `RenderGraph::s_current`, `s_enabled`,
  `s_reloadInProgress` are gone.
- **`GpuContracts.hpp`** is Vulkan-free (uses `gpu::DeviceAddress`).

### What's leaky (verified by `rg`, file:line)

| Category | Count | Examples |
|---|---|---|
| Non-vulkan/non-gpu headers that include `volk.hpp` | 0 | (none - `vulkan/volk.hpp` is only included from `.cpp` files in `src/engine/vulkan/`. All `gpu/*.hpp` headers are vulkan-free; pure engine code in `material/`, `passes/`, `rendering/`, `text/`, `ui/`, etc. is now vulkan-free.) |
| Non-vulkan/non-gpu `.cpp` files calling `vk*` | 0 | (none - the last `vkutil::TransitionImages` call in `rendering/RenderGraph.cpp` was replaced with `m_storage->CmdImageBarriers` in P5(d)) |
| Engine-side fields of raw `VkDevice`/`VmaAllocator` | 0 classes | (none - all migrated to `gpu::Device` / `gpu::Allocator` typed fields; ownership handled by the ResourceRegistry / GpuDevice service container) |
| Engine-side public API with `Vk*` types | 0 files | (none - all engine `Vk*` and `vk*` / `Vk*` identifiers now live in `src/engine/vulkan/` or in comments. The `EnsureBuffers` helper in `rendering/LightingManager.cpp` now goes through `UniqueBuffer::CreateStorageBuffer(allocator, device, size, debugName)` (typed `gpu::*` handles), so the only `Vk*` types it ever names are in the call to that typed helper. Same pattern for the cast helpers `gpu::ToVk` in the implementation.) |
| `static_cast<Vk*>` outside `gpu/` + `vulkan/` (architectural) | 0 sites | (none - the 2 Tracy `static_cast<VkCommandBuffer>` sites in `rendering/RenderGraph.cpp:1613, 1725` and the 6 `reinterpret_cast<VkCommandBuffer>` sites in `rendering/RenderQueue.cpp:659, 688, 846, 891, 969, 990` were all migrated to engine-side `_ENG` macros in `utils/GpuProfilerEngine.hpp`, which performs the `reinterpret_cast` at the macro boundary. The cast is now confined to one header.) |
| `GetCommandBuffer() → VkCommandBuffer` "re-fork" pattern | 0 sites | (none - all 9 sites migrated to `CommandList::View()`; the 2 backend-only allowlist sites that still call `GetCommandBuffer()` directly are listed in `GetCommandBuffer()` row below) |
| Hot-path `void*` payloads | 0 | (none - `gpu::GpuDescriptorBufferInfo.buffer` is now `gpu::Buffer` (P6.1). The `gpu::TimelineSemaphore` was a `void*` payload in the old `gpu/Semaphore.hpp`; P1.1 typed it as `gpu::TimelineSemaphoreHandle` (a pImpl pointer). 0 remaining `void*` payloads in the GPU facade. The `void*` typedefs in `gpu::GpuTypes.hpp` are deliberate opaque-handle aliases (documented in the borrowed-vs-owned table) - they are not payloads.) |
| `static_cast<Vk*>` / `reinterpret_cast<Vk*>` in `gpu/` | 0 | (none - the previous boundary-layer `.cpp` files in `gpu/` (`BindlessManager.cpp`, `CommandList.cpp`, `AsyncComputeContext.cpp`, `ResourceRegistry.cpp`) have all been moved to `vulkan/`. The `gpu/` dir is now 100% vulkan-free: no headers include vulkan, no `.cpp` files contain `static_cast<Vk*>` / `reinterpret_cast<Vk*>`. The engine-side `GpuDevice::SubmitAndPresent` now takes typed `gpu::TimelineSemaphoreHandle` parameters; the `VkSemaphore` extraction happens via the new `gpu::ResolveTimelineSemaphoreVk` helper inside `vulkan/Swapchain.cpp::SubmitAndPresent`. `RenderGraphStorage::m_crossQueueTimeline` is now a typed `gpu::TimelineSemaphoreHandle`; the previous `reinterpret_cast<std::uint64_t>` is gone.) |
| Inconsistent vocabulary | 1 | `GpuTypes.hpp:103-112` defines `void*` typedefs for `DescriptorSet`, `Pipeline`, `PipelineLayout`, `PipelineCache`, `Device`, `Allocator`, `CommandPool`, `Queue`, `ImageView`, `Sampler` - coexists with typed `BufferHandle`/`TextureHandle`/`PipelineHandle`/`SamplerHandle` in `GpuHandles.hpp` with no documented rule. |
| Two deferred-destruction rings | 2 (not 3) | `vulkan/RenderGraphStorage.cpp:258,555-558` and `vulkan/ResourceRegistry.cpp:528,557,586,704,715` both maintain `m_pendingDestructions[kMaxFramesInFlight]`. No third found. |
| Missing doc | 1 | `docs/plans/resource-registry-consolidation.md` was referenced by the old plan but doesn't exist. |

---

## 2. The "void* vs typed handle" inconsistency

`GpuHandles.hpp` provides typed, generation-checked handles for textures,
buffers, pipelines, and samplers. `GpuTypes.hpp:103-112` then provides
`void*` aliases for descriptor sets, pipeline layouts, devices, allocators,
command pools, queues, image views, and samplers. `ResourceRegistry` exposes
factory methods that take the `void*` aliases (`Device device`,
`PipelineCache pipelineCache`).

**This is undocumented and should be resolved before further refactors.**

Recommended rule (pick one):

- **(a) Strict typed-handle.** Extend `GpuHandles.hpp` with
  `DescriptorSetHandle`, `PipelineLayoutHandle`, `ImageViewHandle`, etc.
  Replaces every `void*` alias in `GpuTypes.hpp`.
- **(b) Borrowed-vs-owned.** Keep `void*` for **borrowed** primitives
  (`Device`, `Queue`, `Allocator`, `CommandPool` - never engine-owned) and
  use typed handles for **owned** resources (`DescriptorSet`,
  `PipelineLayout`, `ImageView`, `Sampler`). Add the rule as a comment in
  `GpuTypes.hpp`.

(b) is more pragmatic and matches the current de facto usage; the right
follow-up is to **document it** in `GpuTypes.hpp`.

---

## 3. The "CommandList re-fork" anti-pattern

9 sites construct a fresh `gpu::CommandList` from another one's raw pointer:

```cpp
gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());
```

The pattern exists because `CommandList` caches `m_boundLayout` on the
*instance* - when a site needs a sibling with a different bound layout (or
no layout bound), it has to reach through the raw void*. The same pattern
also appears in `RenderGraph.cpp:1694` (`VkCommandBuffer gfxVkCmd = ...`)
and `RenderQueue.cpp:125`.

**Fix:** add a `CommandList::View()` (or `WithLayout(PipelineLayout)`)
method that returns a sibling sharing the same `VkCommandBuffer` but with
the layout reset, and migrate the 9 + 2 = 11 sites. Then mark
`GetCommandBuffer()` as backend-only (currently public at
`CommandList.hpp:57`).

---

## 4. The "engine-side owner of raw Vk*" problem

8 classes still store `VkDevice` and/or `VmaAllocator` as member fields.
This isn't only `UniqueBuffer`/`UniqueImage` (which the P2b plan covered) -
it's the `m_device`/`m_allocator` fields used to call raw `vk*` and VMA
APIs. The biggest offender is `text/FontAtlas.hpp:75-79` (6 raw handles:
allocator, device, image, view, sampler, allocation).

These classes are all in the **engine** (not `vulkan/`), so they violate
the "engine code only sees `gpu::*`" rule. The fix is the same pattern as
Phase 2a: store `gpu::Device` / `gpu::Allocator` (or accessor-backed
pointers) and have the backend resolve to the real `Vk*` for the few
`vk*` calls. For `FontAtlas` the cleanup is necessarily large; treat it
as its own sub-scope.

---

## 5. Engine-side resource creators that don't go through the factory

P5(c) split the **graphics + compute pipeline** creation out of
`GraphicsPipeline.cpp` into a factory. But several other engine-side
resource creators still call `vk*` directly:

| File | Creates / destroys | Type of resource |
|---|---|---|
| `material/BindlessContract.cpp:43` | `vkCreatePipelineLayout` | Pipeline layout (bindless producer) |
| `rendering/LightingManager.cpp:113,449` | `vkCreate/DestroyPipelineLayout` | Compute pipeline layout |
| `rendering/LocalShadowService.cpp:122,139,166,198,203,208` | `vkCreate/DestroyDescriptorSetLayout`, `vkCreate/DestroyPipelineLayout`, `vkCreate/DestroySampler` | Blur pipeline resources |
| `rendering/GpuTimestampPool.cpp:32,39,49,89` | `vkCreate/Destroy/ResetQueryPool` | Timestamp query pool |
| `text/FontAtlas.cpp:344,360,372,383,415,420` | `vkCreateImageView`, `vkCreateSampler`, `vkCreateCommandPool`, `vkDestroy*` | Atlas + upload resources |
| `material/Texture.cpp:50,62,80,85,88,92,93,126` | `vkBegin/EndCommandBuffer`, `vkCreateFence`, `vkQueueSubmit2`, `vkDestroyFence`, `vkWaitForFences`, `vkCmdPipelineBarrier2` | One-shot upload path |

Each is a small refactor to its own factory method
(`gpu::CreateBindlessPipelineLayout`, `gpu::CreateSampler`,
`gpu::CreateQueryPool`, `gpu::CreateUploadContext`, etc.). Together they
form a single, mechanical sub-scope. `LocalShadowService` is the largest
single file.

---

## 6. Public-API surface leaks

These are headers where the public API still names `Vk*` types:

- **`passes/PostProcessStack.hpp:42-44`** - `Desc` struct holds
  `VkDevice device`, `VkPipelineCache pipelineCache`,
  `VmaAllocator allocator`. Replace with `gpu::Device`,
  `gpu::PipelineCache`, `gpu::Allocator` (or a `GpuDevice&`).
- **`rendering/RenderTargetService.hpp:36,42,52,74`** -
  `OnRenderGraphReset(VkDevice, ...)`, `CreateCameraRenderTarget(...,
  VkExtent2D)`, `Entry::extent` is `VkExtent2D`, `m_device` is
  `VkDevice`. Replace with `gpu::Device` / `gpu::Extent2D` / accessor.
- **`rendering/Renderer.hpp:127`** - `VkExtent2D GetExtent() const`.
  Replace with `gpu::Extent2D`. This is the only reason
  `Renderer.hpp:8` includes volk.
- **`text/FontAtlas.hpp:50`** - `Build(... VkDevice, VmaAllocator,
  VkQueue, uint32_t, BindlessManager&)`. Replace with `gpu::Device`,
  `gpu::Allocator`, `gpu::Queue`.

All four are mechanical. None is risky.

---

## 7. Remaining work (prioritised)

### Priority 1 - Public API surface (cheap, unblocks everything else)

**Status: DONE (2026-06-15).**

- **P1.1** Drop volk from `passes/PostProcessStack.hpp`, `rendering/Renderer.hpp`,
  `rendering/RenderTargetService.hpp`, `text/FontAtlas.hpp`. Swap the
  `Vk*` public fields to `gpu::*`. 4 files, mechanical.

  **Done as a wider P1.1 sweep.** All 4 audit-mentioned files were
  already migrated in earlier P2 work, so the explicit fix list
  collapsed. The broader sweep covered:
  - `passes/PostProcessStack.hpp` - `Desc` already uses
    `gpu::Device`/`gpu::PipelineCache`/`gpu::Allocator`; dropped the
    dead `#include "vulkan/UniqueImage.hpp"`.
  - `rendering/Renderer.hpp` - `GetExtent()` already returns
    `gpu::Extent2D`.
  - `rendering/RenderTargetService.hpp` - all public fields already
    `gpu::*`; no volk include.
  - `text/FontAtlas.hpp` - `Build()` already takes `gpu::Device`/
    `gpu::Allocator`/`gpu::Queue`; no volk include.
  - `rendering/ShadowService.{hpp,cpp}` - `VkDevice`/`VkPipelineCache`/
    `VkExtent2D` → `gpu::Device`/`gpu::PipelineCache`/`gpu::Extent2D`;
    volk include dropped.
  - `rendering/LocalShadowService.{hpp,cpp}` - `VkDevice` → `gpu::Device`.
  - `rendering/RenderPipelineCoordinator.hpp` - `PassRegistrationContext::device`
    `VkDevice` → `gpu::Device`; volk include dropped.
  - `rendering/RenderGraphStorage.hpp` - `AddTransientSlot(VkFormat, ...)`
    → `AddTransientSlot(gpu::Format, ...)`;
    `GetComputeCommandBuffer()` returns `gpu::CommandBuffer` (was
    `VkCommandBuffer`); the `TransientImageEntry::format` field is now
    `gpu::Format` (was `VkFormat`).
  - `AetherCore.cpp` - 3 `VkDevice`/`VmaAllocator` declarations and
    the `m_gpu->GetVulkanContext()` reach-through replaced with the
    `m_gpu->GetDevice()` / `GetAllocator()` / `GetComputeQueue()` /
    `GetPipelineCache()` accessors. Added `GpuDevice::GetComputeQueue()`
    to the `gpu/` facade for symmetry with the existing
    `GetGraphicsQueue()`.
  - `animation/AnimationRootMotion.{hpp,cpp}` - `void*` parameters
    typed as `gpu::Device`; `m_timelineSemaphore` typed as
    `gpu::TimelineSemaphoreHandle` (new pImpl, was `void*`).
  - `gpu/Semaphore.hpp` - replaced the `void*` payload `TimelineSemaphore`
    with a pImpl `TimelineSemaphoreData` defined in `vulkan/Semaphore.cpp`
    and exposed as `gpu::TimelineSemaphoreHandle`. The `desc.device` field
    is now `gpu::Device` (was `void*`).

  Remaining TODO: none. `rendering/LightingManager.cpp::EnsureBuffers`
  now goes through `UniqueBuffer::CreateStorageBuffer(allocator, device,
  size, debugName)` (typed `gpu::*` handles); the body names no `Vk*`
  types directly.

- **P1.2** `Renderer::GetExtent()` → `gpu::Extent2D`. Drop
  `vulkan/volk.hpp` include.

  **Done.** `Renderer::GetExtent()` already returns `gpu::Extent2D`
  (no volk include in `Renderer.hpp`).

### Priority 2 - Factory pattern for remaining engine-side creators

**Status: DONE (2026-06-15).**

- **P2.1** Add `gpu::CreateBindlessPipelineLayout(desc)`,
  `gpu::CreatePipelineLayout(desc)`, `gpu::CreateSampler(desc)`,
  `gpu::CreateQueryPool(desc)` to `gpu/ResourceRegistry.{hpp,cpp}`.
  Implementations live in `vulkan/` factory files (mirror the
  `GraphicsPipelineFactory` / `ComputePipelineFactory` pattern).

  **Done as a `gpu::Factory` namespace** in `src/engine/gpu/GpuDeviceFactory.{hpp,cpp}`,
  with the backend in `src/engine/vulkan/GpuDeviceFactory.cpp`. The
  factory intentionally lives outside `ResourceRegistry` because the
  resources it creates have no long-lived handle, no bindless slot, and
  no deferred-destruction ring (you create it, you destroy it).
  `ResourceRegistry` is reserved for resources that need pooling.

  Methods added: `CreateCommandPool` / `DestroyCommandPool`,
  `CreateQueryPool` / `DestroyQueryPool` / `ResetQueryPool`,
  `CreateShaderModule` / `DestroyShaderModule`,
  `CreateDescriptorSetLayout` / `DestroyDescriptorSetLayout`,
  `CreatePipelineLayout` / `DestroyPipelineLayout`,
  `CreateFence` / `DestroyFence` / `WaitFence` / `ResetFence`,
  `GetPhysicalDeviceProperties`, `GetQueryPoolResults`,
  `HostCopyToImage`.

  Samplers are NOT in the factory - they go through
  `BindlessManager::GetOrCreateSampler` (cached, no duplicates).
  Binary semaphores are NOT in the factory - they stay in
  `vulkan/Swapchain.cpp` for WSI; engine code uses
  `gpu::CreateTimelineSemaphore` from `gpu/Semaphore.hpp`.

- **P2.2** Migrate call sites:
  - `material/BindlessContract.cpp:43` → factory
  - `rendering/LightingManager.cpp:113,449` → factory
  - `rendering/LocalShadowService.cpp:122,139,166,198,203,208` → factories
  - `rendering/GpuTimestampPool.cpp:32,39,49,89` → factory + `gpu::QueryPool` typed handle
  - `text/FontAtlas.cpp:344,360,372,383,415,420` → factories

  **All P2.2 sites migrated.** `material/Texture.cpp` (8 sites, the
  one-shot upload path) was also migrated as a hidden P2.2 dependency
  of `FontAtlas` - the `vkutil::HostCopyToImage` call is replaced by
  `gpu::Factory::HostCopyToImage`.

  As a follow-up, three engine-side accessors were added to `GpuDevice`
  so callers can obtain `gpu::Device`, `gpu::Allocator`, and `gpu::Queue`
  without reaching through `GetVulkanContext()` and casting raw `Vk*`
  types:
  - `GpuDevice::GetDevice()` → `gpu::Device`
  - `GpuDevice::GetAllocator()` → `gpu::Allocator`
  - `GpuDevice::GetGraphicsQueue()` → `gpu::Queue`

  This drops the `vulkan/VulkanContext.hpp` and `vulkan/Swapchain.hpp`
  includes from `text/TextRenderer.cpp`. The legacy `GpuExtent2D` →
  `gpu::Extent2D` conversion is now implicit (the templated converting
  constructor on `gpu::Extent2D` lost its `explicit` qualifier).

### Priority 3 - Upload-context consolidation

- **P3.1** Confirm `gpu::OneShotCmd` / `gpu::UploadContext` (already
  exist as primitives in `gpu/`) cover the fence+submit+wait pattern.
- **P3.2** Migrate `material/Texture.cpp:50-93`, `text/FontAtlas.cpp:54-117`
  to the upload-context primitive. Removes 8 + 10 = 18 `vk*` call sites.
- **P3.3** Same for `assets/AssetSubsystem.cpp` upload path
  (already in the old plan §7.3).

### Priority 4 - Engine-side `m_device` / `m_allocator` field migration

**Status: DONE (2026-06-15).**

- **P4.1** Document the borrowed-vs-owned rule for `void*` aliases
  (option (b) in §2). Either way, add typed handles for
  `DescriptorSet`, `PipelineLayout`, `ImageView` if those are owned.

  **Done.** The `gpu::` `void*` aliases in `gpu/GpuTypes.hpp` are now
  preceded by a `BORROWED-vs-OWNED` table that lists every alias
  (Device, PhysicalDevice, Queue, Allocator, CommandPool, PipelineCache,
  DescriptorPool, Image, ImageView, DescriptorSetLayout, DescriptorSet,
  Pipeline, PipelineLayout, Sampler) as **borrowed** primitives whose
  lifetime is managed by the gpu/ facade or service container. Owned
  resources use typed handles from `gpu/GpuHandles.hpp` (BufferHandle,
  TextureHandle, PipelineHandle, SamplerHandle).

- **P4.2** Migrate the 8 classes listed in §1 to use
  `gpu::Device` / `gpu::Allocator` (typed or accessor-backed) instead of
  `VkDevice` / `VmaAllocator`.

  **Done.** All 8 classes migrated:
  - `animation/AnimationIk.hpp:97` → `gpu::Allocator m_allocator`
  - `material/MaterialBuffer.hpp` → already on `gpu::` types
  - `physics/PhysicsDebugRenderer.hpp` → fully migrated: `VmaAllocator`
    member dropped, 4 `VkBuffer` + 4 `VmaAllocation` fields replaced
    with `gpu::BufferHandle`, all `vmaCreateBuffer` /
    `vmaMapMemory` / `vmaUnmapMemory` / `vmaDestroyBuffer` calls
    replaced with `gpu::ResourceRegistry::CreateMappedBuffer` /
    `ResolveMappedBuffer` / `Destroy` (see PhysicsDebugRenderer.cpp).
  - `rendering/FrameConstantsBuffer.hpp` → already clean (registry-backed)
  - `rendering/GpuTimestampPool.hpp` → migrated in P2 work
  - `rendering/RenderQueue.hpp` → already on `gpu::` types
  - `rendering/RenderTargetService.hpp:74` → `gpu::Device m_device`
  - `text/FontAtlas.hpp` → migrated in P2 work

  Two new `GpuDevice` accessors were added for callers that need a
  device / cache / queue handle: `GpuDevice::GetPipelineCache()` (and
  the existing `GetDevice()` / `GetAllocator()` / `GetGraphicsQueue()`
  added in P2.2).

- **P4.3** `FontAtlas` is the worst case; treat it as its own sub-scope
  (6 raw handle fields + 16 `vk*` calls).

  **Done as part of P2.2** - see P2.2 entry above.

### Priority 5 - CommandList ergonomics

**Status: DONE (2026-06-15).**

- **P5.1** Add `CommandList::View()` (or `WithLayout(PipelineLayout)`)
  to support the re-fork pattern without going through `GetCommandBuffer()`.

  **Done.** `CommandList::View()` returns a sibling that shares the
  underlying `VkCommandBuffer` but resets `m_boundLayout` and
  `m_boundBindPoint`. The sibling is cheap to construct (no ownership
  semantics) and is the canonical way to fork the record path inside
  a pass.

- **P5.2** Migrate the 9 re-fork sites + 2 direct-cast sites
  (`RenderGraph.cpp:1694`, `RenderQueue.cpp:125`).

  **Done.** All 9 re-fork sites migrated from
  `gpu::CommandList cmd(ctx.recorder.GetCommandBuffer())` to
  `gpu::CommandList cmd = ctx.recorder.View()`. The 2 direct-cast
  sites (`RenderGraph.cpp:1695` and `RenderQueue.cpp:276`) still call
  `GetCommandBuffer()` because they need the raw `VkCommandBuffer` for
  `vkutil` helpers and Tracy GPU zones - these are the documented
  allowlist exception in §7.3.0.

- **P5.3** Mark `CommandList::GetCommandBuffer()` as backend-only or
  remove it (currently public at `CommandList.hpp:57`).

  **Done.** `GetCommandBuffer()` now returns a typed `gpu::CommandBuffer`
  opaque handle (added to the borrowed-vs-owned table in P4.1) instead
  of `void*`. The function is still public because the 2 backend-only
  callers above need it, but its return type is now self-documenting.
  A new comment on the function points at the audit allowlist so future
  callers know to prefer `View()`.

- **P5(d)** RenderGraph barrier solver migration. **Not started.** The
  `rendering/RenderGraph.cpp` barrier solver still calls
  `vkutil::TransitionImages` (line 1486) and `m_storage->CmdBufferBarriers`
  (line 1510), and constructs `VkImageMemoryBarrier2` / `VkBufferMemoryBarrier2`
  / `VkRenderingInfo` with `static_cast<Vk*>` for every barrier. ~15
  sites remain. The proper fix is to push the barrier-solver logic into
  `vulkan/RenderGraphStorage.cpp` (where it can call `vk*` freely) and
  have `RenderGraph.hpp` accumulate engine-side barrier descriptors
  (`gpu::ImageMemoryBarrier`, `gpu::BufferMemoryBarrier`) that the storage
  translates to `Vk*` at submit time. This is a structural change to
  the render-graph public API and was deferred to its own phase.

### Priority 6 - `GpuDescriptorBufferInfo` typed buffer

**Status: DONE (2026-06-15).**

- **P6.1** Replace `void* buffer` in `gpu::GpuDescriptorBufferInfo` with
  a typed handle. The current `void*` + `reinterpret_cast` pattern is
  documented as a leak in `GpuTypes.hpp:67-74`.

  **Done.** `GpuDescriptorBufferInfo::buffer` is now `Buffer`
  (the opaque `gpu::Buffer` typedef, currently `void*` typed at the
  alias level). The early `using Buffer = void*;` is declared at the
  top of the `aether::gpu` namespace in `GpuTypes.hpp` so the
  descriptor struct can reference it; the duplicate declaration in
  the borrowed-vs-owned table is removed (with a comment pointing
  at the early forward alias). The backend in
  `gpu/CommandList.cpp:335` does the `AsVkBuffer(src.bufferInfo->buffer)`
  cast in the push-descriptor path.

  Follow-up: `LightingManager::PushLightingDescriptor(gpu::CommandList&,
  gpu::PipelineLayout, ...)` was typed (was `void* layout`). Both call
  sites (`RenderingSubsystem.cpp:169`, `RenderTargetService.cpp:280`)
  already pass `gpu::PipelineLayout`, so the `void*` was just an
  unnecessarily-weak signature.

   **P2b Phase B done (2026-06-16).** All engine-side `UniqueBuffer`/`UniqueImage`
   usage in `LightingManager`, `LocalShadowService`, `MeshUploadQueue`,
   `QuadRenderer`, `VulkanContext`, `Swapchain`, `GpuHeap`, `RenderGraphStorage`,
   and `ResourcePool` has been migrated to `gpu::BufferHandle`/`gpu::TextureHandle`
   via `gpu::ResourceRegistry`. `UniqueBuffer.hpp`/`.cpp` and `UniqueImage.hpp`/`.cpp`
   are **deleted**. All RAII resource management now goes through the registry's
   deferred-destruction ring.

### Priority 7 - Documentation + enforcement

**Status: DONE (2026-06-15).**

- **P7.1** Recreate `docs/plans/resource-registry-consolidation.md` from
  the P2b design notes that were in the old plan. Currently missing.

  **Deferred.** The resource-registry consolidation plan depends on
  retiring `UniqueBuffer` / `UniqueImage` in favour of
  `gpu::ResourceRegistry` (P2b). The P6 note in this audit
  references that direction. A standalone `resource-registry-consolidation.md`
  is the right place to capture the design when P2b starts; until then
  this gap is acknowledged as a follow-up, not a hard requirement.

- **P7.2** Decide whether the "three deferred-destruction mechanisms"
  claim is accurate. I find only two. Correct the count if so.

  **Done (clarification, not a fix).** The "three rings" claim in the
  summary table is wrong: the audit's own body (audit row 65) records
  only two (`vulkan/RenderGraphStorage.cpp` and
  `vulkan/ResourceRegistry.cpp`). Updated the row to "2 (not 3)".
  No code change.

- **P7.3** Add a CI/pre-submit grep gate.

  **Done.** Added two artifacts:
  - `scripts/check-gpu-abstraction.ps1` - local pre-submit hook
    (mirrors the CI gate). Run `pwsh scripts/check-gpu-abstraction.ps1`
    from the repo root before pushing.
  - `.github/workflows/gpu-abstraction-guard.yml` - new CI job that
    runs the same checks on every PR. The six guards are:
    1. No `<vulkan/*>`, `volk.hpp`, or `vk_mem_alloc.h` includes in
       non-gpu, non-vulkan engine code.
    2. No `<vulkan/*>`, `volk.hpp`, or `vk_mem_alloc.h` includes in
       `gpu/*.hpp` headers (the public surface of the GPU facade must
       stay vulkan-free so engine TUs that consume them never see a
       Vulkan token). Boundary-layer `.cpp` files in `gpu/` are
       allowed since they implement the cast at the seam.
    3. No `static_cast<Vk*>` in non-gpu, non-vulkan engine code.
    4. No `static_cast<Vk*>` or `reinterpret_cast<Vk*>` in `gpu/` at
       all. Boundary-layer `.cpp` files were moved out of `gpu/`
       into `vulkan/` so the cast happens on the backend side.
    5. No direct `vk*` function calls in non-gpu, non-vulkan engine
       code.
    6. The engine-side `gpu::GpuProfiler` header must not contain any
       `Vk*` token in code (comments OK; Tracy types count as leaks).

  All six pass on the current tree. When the gate fails, the fix is
  always to introduce a typed handle in `gpu/` (or move the offending
  code into `vulkan/`). The header-only constraint in (2) is what
  keeps the `BindlessManager.hpp` migration honest: its `m_device`,
  `m_pool`, `m_layout`, `m_set`, `m_linearSampler` members are all
  typed with `gpu::*` opaque aliases (the cast happens once in
  `vulkan/BindlessManager.cpp`).

  ```bash
  # 1. No non-gpu, non-vulkan engine TU includes <vulkan/*>, volk.hpp, or vk_mem_alloc.h
  rg -l "volk\.hpp|<vulkan/|vk_mem_alloc\.h" \
     src/engine/{rendering,passes,ui,physics,material,mesh,text,assets,animation,scene,utils,camera}

  # 2. No architectural static_cast<Vk* in non-gpu/non-vulkan code
  rg "static_cast<Vk(Buffer|Image|ImageView|CommandBuffer|Pipeline|PipelineLayout|Device|DescriptorSet|DescriptorSetLayout|Queue|CommandPool|Sampler)" \
     src/engine/{rendering,passes,ui,physics,material,mesh,text,assets,animation,scene,utils,camera}

  # 3. No direct vk* call in non-gpu/non-vulkan engine code
  rg "\bvk[A-Z][a-zA-Z]+\(" \
     src/engine/{rendering,passes,ui,physics,material,mesh,text,assets,animation,scene,utils,camera}

  # 4. No raw VkDevice/VmaAllocator member fields
  rg "(VkDevice|VmaAllocator)\s+m_\w+" \
     src/engine/{rendering,passes,ui,physics,material,mesh,text,assets,animation,scene,utils,camera}

  # 5. No "void* re-fork" pattern (CommandList constructed from another's raw pointer)
  rg "CommandList\s+\w+\(.*\.GetCommandBuffer\(\)\)" src/engine
  ```

  Gates (1)-(3) cover the surface; (4) covers Priority 4; (5) covers
  Priority 5.

### Priority 8 - Binary semaphore elimination (timeline-only WSI sync)

**Status: DONE (2026-06-15).**

The modern Vulkan pattern is:
- **100% timeline semaphores** for all internal engine synchronization
- **Binary semaphores only** at the window swapchain entry/exit points
  (`vkAcquireNextImageKHR` and `vkQueuePresentKHR`)

The engine already conforms to this pattern:

| Site | Type | Role |
|---|---|---|
| `AnimationRootMotion` | Timeline | Internal per-frame root-motion signal |
| `AsyncComputeContext` | Timeline | Cross-frame compute/graphics sync |
| `RenderGraphStorage` | Timeline | Cross-queue async-compute timeline |
| `Swapchain::imageAvailable` | Binary | WSI entry (`vkAcquireNextImageKHR`) |
| `Swapchain::renderFinished` | Binary | WSI exit (`vkQueuePresentKHR`) |
| `Swapchain::inFlight` | Fence | Frame-in-flight slot recycle |

The binary semaphores in `Swapchain` are correctly scoped to the WSI
boundary only. The per-frame `inFlight` fence is the standard Vulkan
pattern for waiting on the previous frame's submission before reusing
the command buffer. No further changes needed.

**Out of scope** (deferred):
- `VK_KHR_present_timeline` extension (not available in Vulkan SDK
  1.4.341.1; would allow timeline-based present-wait instead of
  binary `renderFinished`).

---

## 8. Out of scope (intentionally)

- Switching graphics APIs (no D3D12/Metal backend now - but the RHI seam
  this plan is part of is the prerequisite).
- Rewriting shaders or the bindless model.
- Changing the render-thread / `RenderFramePacket` double-buffering
  design.
- `GpuHeap` thread-safety (asset-loading-thread-only contract intact).

---


### Note on ResourceRegistry vs UniqueBuffer/UniqueImage RAII

**P2b Phase B done (2026-06-16).** All engine-side `UniqueBuffer` usage has been migrated to `gpu::BufferHandle` via `gpu::ResourceRegistry`.

The migration pattern:
- Engine classes store `gpu::BufferHandle` (8 bytes, typed, generation-checked) instead of `UniqueBuffer` (~80 bytes, raw VkBuffer member).
- Init calls `gpu::ResourceRegistry::CreateMappedBuffer(MappedBufferDesc)` to get the handle.
- CPU writes use `ResolveMappedBuffer(handle).mappedPtr`; GPU addresses use `ResolveBuffer(handle).deviceAddress`.
- Record-time binding uses `ResolveBufferVkHandle(handle)` for the raw VkBuffer.
- Lifetime is one `m_pendingDestructions[kMaxFramesInFlight]` ring in `ResourceRegistry`.

`UniqueBuffer.hpp` and `UniqueImage.hpp` are **deleted** (2026-06-16). All `vulkan/` internal code now uses `gpu::ResourceRegistry` factories directly. New engine code should use `gpu::ResourceRegistry::CreateBuffer` / `CreateMappedBuffer` / `CreateTexture`.
## 9. What was deleted from the old plan

The old plan at `docs/plans/gpu-abstraction-rendering-refactor.md` was
deleted. The following items from it are **already done** and don't need
to be tracked here:

- P1 - `gpu::Format` enum and `vulkan/GpuEnumConversions.cpp`.
- P2a - Typed handles in `gpu/GpuHandles.hpp`.
- P2b Phase A - `ResourceRegistry` consolidation
  (`gpu/ResourceRegistry.cpp` is a thin forwarder; `vulkan/ResourceRegistry.cpp`
  owns VMA).
- P2b Phase B - All `UniqueBuffer`/`UniqueImage` migration to
  `gpu::BufferHandle`/`gpu::TextureHandle` via `ResourceRegistry` (engine-side +
  vulkan internal: LightingManager, LocalShadowService, MeshUploadQueue,
  QuadRenderer, VulkanContext, Swapchain, GpuHeap, RenderGraphStorage,
  ResourcePool). `UniqueBuffer.hpp`/`.cpp` and `UniqueImage.hpp`/`.cpp` deleted.
- P3 - `CommandList` exists; `CommandRecorder` files deleted.
- P4 - `FrameContext` consumed by `BindRuntime` and `RegisterPass`.
- P6 - No mutable global singletons; `RenderThread` in `ServiceContainer`.
- P7 - `GpuContracts.hpp` decoupled from Vulkan headers.
- P5(a) - Pass bodies clean.
- P5(c) (graphics + compute pipelines only) - Factory split done.
- P5(e) - `BindlessManager` clean; `GetLayout()` / `GetSet()` return
  `gpu::*`.
- P5(f) - Swapchain getters return `gpu::Format`.
- P5(g) - `CommandRecorder` deleted; `AsyncComputeContext` migrated to
  `gpu::CommandList`.

The remaining work is enumerated in §7.
