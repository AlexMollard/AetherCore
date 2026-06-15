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
| Non-vulkan/non-gpu headers that include `volk.hpp` | 14 | `material/BindlessContract.hpp:7`, `material/MaterialBuffer.hpp:8`, `passes/PostProcessStack.hpp:6`, `rendering/GpuTimestampPool.hpp:7`, `rendering/Renderer.hpp:8`, `rendering/RenderPipelineCoordinator.hpp:4`, `rendering/RenderQueue.hpp:11`, `rendering/RenderTargetService.hpp:7`, `rendering/ShadowAtlasManager.hpp:7`, `rendering/ShadowService.hpp:6`, `text/FontAtlas.hpp:8`, `ui/QuadRenderer.hpp:9`, `utils/GpuProfiler.hpp:11`, `utils/GpuProfiler.hpp:4` (doc) |
| Non-vulkan/non-gpu `.cpp` files calling `vk*` | 8 | `material/BindlessContract.cpp:43` (`vkCreatePipelineLayout`), `material/Texture.cpp:50,62,80,85,88,92,93,126` (8 vk* sites), `rendering/GpuTimestampPool.cpp:32,39,49,89` (4 vk* sites), `rendering/LightingManager.cpp:113,449` (2 vk* sites), `rendering/LocalShadowService.cpp:122,139,166,198,203,208` (6 vk* sites), `rendering/RenderGraph.cpp:1488,1512` (vkutil barrier calls), `text/FontAtlas.cpp:54,76,80,87,91,104,109,112,114,116,344,360,372,383,415,420` (~16 vk* sites), `ui/QuadRenderer.cpp:53,107` (2 vk* sites) |
| Engine-side fields of raw `VkDevice`/`VmaAllocator` | 8 classes | `animation/AnimationIk.hpp:89`, `material/MaterialBuffer.hpp:55-56`, `physics/PhysicsDebugRenderer.hpp:127`, `rendering/FrameConstantsBuffer.hpp:42-43`, `rendering/GpuTimestampPool.hpp:52`, `rendering/RenderQueue.hpp:232-233`, `rendering/RenderTargetService.hpp:74`, `text/FontAtlas.hpp:75-79` (worst: 6 raw handles) |
| Engine-side public API with `Vk*` types | 5 files | `passes/PostProcessStack.hpp:42-44` (`VkDevice`/`VkPipelineCache`/`VmaAllocator` in `Desc`), `rendering/RenderTargetService.hpp:36,42,52,74`, `rendering/Renderer.hpp:127` (`VkExtent2D GetExtent()`), `text/FontAtlas.hpp:50` (`VkDevice`/`VmaAllocator`/`VkQueue` in `Build`) |
| `static_cast<Vk*>` outside `gpu/` + `vulkan/` (architectural) | ~12 sites | `rendering/RenderGraph.cpp:35,263,305,321,334,386,1372,1376,1427-1430,1478-1481,1503-1506,1522,1552,1644-1647,1694`, `rendering/RenderQueue.cpp:125,187,192,197,204-208,330,431,462,495,602,724,758,759,766,768,794,952`, `rendering/LightingManager.cpp:113,440`, `rendering/LocalShadowService.cpp:198,503`, `animation/*` (passes `VkCommandPool` / `VkDeviceSize` to `m_heap.Upload`) |
| `GetCommandBuffer() → VkCommandBuffer` "re-fork" pattern | 9 sites | `passes/PostProcessStack.cpp:141`, `rendering/LightingManager.cpp:543,577`, `rendering/LocalShadowService.cpp:480,527,579`, `rendering/RenderTargetService.cpp:283`, `rendering/ShadowService.cpp:139`, `ui/QuadRenderer.cpp:115` |
| Hot-path `void*` payloads | 1 | `gpu::GpuDescriptorBufferInfo.buffer` (`GpuTypes.hpp:71`) is documented as a leak: backend `reinterpret_cast`s. |
| Inconsistent vocabulary | 1 | `GpuTypes.hpp:103-112` defines `void*` typedefs for `DescriptorSet`, `Pipeline`, `PipelineLayout`, `PipelineCache`, `Device`, `Allocator`, `CommandPool`, `Queue`, `ImageView`, `Sampler` — coexists with typed `BufferHandle`/`TextureHandle`/`PipelineHandle`/`SamplerHandle` in `GpuHandles.hpp` with no documented rule. |
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
  (`Device`, `Queue`, `Allocator`, `CommandPool` — never engine-owned) and
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
*instance* — when a site needs a sibling with a different bound layout (or
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
This isn't only `UniqueBuffer`/`UniqueImage` (which the P2b plan covered) —
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

- **`passes/PostProcessStack.hpp:42-44`** — `Desc` struct holds
  `VkDevice device`, `VkPipelineCache pipelineCache`,
  `VmaAllocator allocator`. Replace with `gpu::Device`,
  `gpu::PipelineCache`, `gpu::Allocator` (or a `GpuDevice&`).
- **`rendering/RenderTargetService.hpp:36,42,52,74`** —
  `OnRenderGraphReset(VkDevice, ...)`, `CreateCameraRenderTarget(...,
  VkExtent2D)`, `Entry::extent` is `VkExtent2D`, `m_device` is
  `VkDevice`. Replace with `gpu::Device` / `gpu::Extent2D` / accessor.
- **`rendering/Renderer.hpp:127`** — `VkExtent2D GetExtent() const`.
  Replace with `gpu::Extent2D`. This is the only reason
  `Renderer.hpp:8` includes volk.
- **`text/FontAtlas.hpp:50`** — `Build(... VkDevice, VmaAllocator,
  VkQueue, uint32_t, BindlessManager&)`. Replace with `gpu::Device`,
  `gpu::Allocator`, `gpu::Queue`.

All four are mechanical. None is risky.

---

## 7. Remaining work (prioritised)

### Priority 1 — Public API surface (cheap, unblocks everything else)

- **P1.1** Drop volk from `passes/PostProcessStack.hpp`, `rendering/Renderer.hpp`,
  `rendering/RenderTargetService.hpp`, `text/FontAtlas.hpp`. Swap the
  `Vk*` public fields to `gpu::*`. 4 files, mechanical.
- **P1.2** `Renderer::GetExtent()` → `gpu::Extent2D`. Drop
  `vulkan/volk.hpp` include.

### Priority 2 — Factory pattern for remaining engine-side creators

- **P2.1** Add `gpu::CreateBindlessPipelineLayout(desc)`,
  `gpu::CreatePipelineLayout(desc)`, `gpu::CreateSampler(desc)`,
  `gpu::CreateQueryPool(desc)` to `gpu/ResourceRegistry.{hpp,cpp}`.
  Implementations live in `vulkan/` factory files (mirror the
  `GraphicsPipelineFactory` / `ComputePipelineFactory` pattern).
- **P2.2** Migrate call sites:
  - `material/BindlessContract.cpp:43` → factory
  - `rendering/LightingManager.cpp:113,449` → factory
  - `rendering/LocalShadowService.cpp:122,139,166,198,203,208` → factories
  - `rendering/GpuTimestampPool.cpp:32,39,49,89` → factory + `gpu::QueryPool` typed handle
  - `text/FontAtlas.cpp:344,360,372,383,415,420` → factories

### Priority 3 — Upload-context consolidation

- **P3.1** Confirm `gpu::OneShotCmd` / `gpu::UploadContext` (already
  exist as primitives in `gpu/`) cover the fence+submit+wait pattern.
- **P3.2** Migrate `material/Texture.cpp:50-93`, `text/FontAtlas.cpp:54-117`
  to the upload-context primitive. Removes 8 + 10 = 18 `vk*` call sites.
- **P3.3** Same for `assets/AssetSubsystem.cpp` upload path
  (already in the old plan §7.3).

### Priority 4 — Engine-side `m_device` / `m_allocator` field migration

- **P4.1** Document the borrowed-vs-owned rule for `void*` aliases
  (option (b) in §2). Either way, add typed handles for
  `DescriptorSet`, `PipelineLayout`, `ImageView` if those are owned.
- **P4.2** Migrate the 8 classes listed in §1 to use
  `gpu::Device` / `gpu::Allocator` (typed or accessor-backed) instead of
  `VkDevice` / `VmaAllocator`.
- **P4.3** `FontAtlas` is the worst case; treat it as its own sub-scope
  (6 raw handle fields + 16 `vk*` calls).

### Priority 5 — CommandList ergonomics

- **P5.1** Add `CommandList::View()` (or `WithLayout(PipelineLayout)`)
  to support the re-fork pattern without going through `GetCommandBuffer()`.
- **P5.2** Migrate the 9 re-fork sites + 2 direct-cast sites
  (`RenderGraph.cpp:1694`, `RenderQueue.cpp:125`).
- **P5.3** Mark `CommandList::GetCommandBuffer()` as backend-only or
  remove it (currently public at `CommandList.hpp:57`).

### Priority 6 — `GpuDescriptorBufferInfo` typed buffer

- **P6.1** Replace `void* buffer` in `gpu::GpuDescriptorBufferInfo` with
  `BufferHandle buffer` (or a `const UniqueBuffer&`-like view if a
  `BufferHandle` is too heavy for a hot path). The current
  `void*` + `reinterpret_cast` pattern is documented as a leak in
  `GpuTypes.hpp:67-74`.

### Priority 7 — Documentation + enforcement

- **P7.1** Recreate `docs/plans/resource-registry-consolidation.md` from
  the P2b design notes that were in the old plan. Currently missing.
- **P7.2** Decide whether the "three deferred-destruction mechanisms"
  claim is accurate. I find only two. Correct the count if so.
- **P7.3** Add a CI/pre-submit grep gate (per the old plan's P8):

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

---

## 8. Out of scope (intentionally)

- Switching graphics APIs (no D3D12/Metal backend now — but the RHI seam
  this plan is part of is the prerequisite).
- Rewriting shaders or the bindless model.
- Changing the render-thread / `RenderFramePacket` double-buffering
  design.
- `GpuHeap` thread-safety (asset-loading-thread-only contract intact).

---


### Note on ResourceRegistry vs UniqueBuffer/UniqueImage RAII

The engine-side UniqueBuffer::CreateMapped/UniqueImage::Create overloads added in this batch are a **bridge migration**, not a registry migration. They fix the k* token leak at the call site (the oid* ↔ Vk* cast now lives inside ulkan/UniqueBuffer.cpp / ulkan/UniqueImage.cpp), but the engine code still uses the UniqueBuffer RAII path with its own 3-frame deferred-destruction ring.

The proper end-state is P2b Phase B (docs/plans/resource-registry-consolidation.md):
- Engine classes store gpu::BufferHandle / gpu::TextureHandle (8 bytes, typed, generation-checked) instead of UniqueBuffer / UniqueImage (~80 bytes, raw VkBuffer / VkImage member).
- Init calls gpu::ResourceRegistry::CreateMappedBuffer(MappedBufferDesc) / CreateTexture(TextureDesc) to get the handle.
- CPU writes use ResolveMappedBuffer(handle).mappedPtr; GPU addresses use ResolveBuffer(handle).deviceAddress.
- Record-time binding uses ResolveBufferVkHandle(handle) for the raw VkBuffer.
- Lifetime is one m_pendingDestructions[kMaxFramesInFlight] ring (in ResourceRegistry), not three.

Each engine-side overload added in this batch has a // TODO(audit/P2b) comment marking it for the registry migration. New engine code should prefer gpu::ResourceRegistry factories directly.
## 9. What was deleted from the old plan

The old plan at `docs/plans/gpu-abstraction-rendering-refactor.md` was
deleted. The following items from it are **already done** and don't need
to be tracked here:

- P1 — `gpu::Format` enum and `vulkan/GpuEnumConversions.cpp`.
- P2a — Typed handles in `gpu/GpuHandles.hpp`.
- P2b Phase A — `ResourceRegistry` consolidation
  (`gpu/ResourceRegistry.cpp` is a thin forwarder; `vulkan/ResourceRegistry.cpp`
  owns VMA).
- P3 — `CommandList` exists; `CommandRecorder` files deleted.
- P4 — `FrameContext` consumed by `BindRuntime` and `RegisterPass`.
- P6 — No mutable global singletons; `RenderThread` in `ServiceContainer`.
- P7 — `GpuContracts.hpp` decoupled from Vulkan headers.
- P5(a) — Pass bodies clean.
- P5(c) (graphics + compute pipelines only) — Factory split done.
- P5(e) — `BindlessManager` clean; `GetLayout()` / `GetSet()` return
  `gpu::*`.
- P5(f) — Swapchain getters return `gpu::Format`.
- P5(g) — `CommandRecorder` deleted; `AsyncComputeContext` migrated to
  `gpu::CommandList`.

The remaining work is enumerated in §7.
