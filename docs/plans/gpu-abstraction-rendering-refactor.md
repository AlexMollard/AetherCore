# Plan: Clean GPU Abstraction Layer + Modern Rendering Refactor

> **Audience:** an LLM coding agent (or human) executing this refactor incrementally.
> **Repo:** AetherCore. C++26 (Clang) / C++23 (MSVC). Vulkan 1.3, volk, VMA, vk-bootstrap.
> **Scope:** `src/engine/gpu/`, `src/engine/vulkan/`, `src/engine/rendering/`, `src/engine/passes/`.
> **Read first:** `docs/ARCHITECTURE.md` (§ GPU/Vulkan two-layer, § render graph, § 10 invariants), `AGENTS.md` (conventions, "Do Not" list).

This plan is **incremental and non-breaking per phase**. Every phase must compile and run before the next begins. Do **not** attempt a big-bang rewrite. Each phase lists: Goal, Why, Files, Steps, Acceptance, Risk.

---

## 0. Problem Statement (evidence-backed)

The codebase claims a "two-layer GPU abstraction": engine code uses `gpu/` types, only `vulkan/` includes `<vulkan/*>`. **This boundary is only enforced at device creation.** Everything downstream leaks raw Vulkan.

### Concrete leaks & hazards found in source

| # | Problem | Evidence |
|---|---------|----------|
| L1 | `CommandRecorder` (engine-facing render layer) is entirely raw Vulkan | `rendering/CommandRecorder.hpp` — `VkCommandBuffer`, `VkBuffer`, `VkPipelineLayout`, `VkDescriptorSet`, `VkPipelineStageFlags2`, `VkAccessFlags2` in the public API |
| L2 | `RenderGraph` public API is raw Vulkan | `rendering/RenderGraph.hpp` — `VkFormat`, `VkImageUsageFlags`, `VkImageLayout`, `VkAttachmentLoadOp`, `VkClearValue`, `FrameTarget` is all `VkImage`/`VkImageView` |
| L3 | `GpuFormat` enum has only **6 values**, so passes bypass it with raw `VkFormat` everywhere | `gpu/GpuTypes.hpp:7-15` vs `VkFormat` usage in `GraphicsPipeline::Desc`, `Renderer::GetColorFormat()` |
| L4 | `Renderer` (a "service") exposes `VkFormat`/`VkExtent2D` | `rendering/Renderer.hpp:124-126` |
| L5 | Passes take raw-Vulkan callbacks | `passes/ForwardPass.hpp:36` — `std::function<void(VkCommandBuffer, VkPipelineLayout)>` |
| L6 | Mutable global singleton with public field | `rendering/RenderGraph.hpp:373` — `static RenderGraph* s_current;` (public), plus free `GetCurrentRenderGraph()` |
| L7 | Hand-rolled barrier/layout state machine, easy to desync | `rendering/RenderGraph.hpp` — `ResourceState`, `CompiledBarrier`, manual `oldLayout/newLayout/srcStage/dstStage` tracking |
| L8 | Parameter-explosion wiring; fragile, order-dependent, easy to mis-pass | `rendering/RenderTargetService.hpp:32-42` — `BindRuntime(...)` takes **11 params / 7 raw pointers**; `passes/ForwardPass.hpp:31-38` takes 8 |
| L9 | Per-feature `static bool s_enabled` / `static` reload flags = hidden global state, not thread-safe with the render thread | `passes/ForwardPass.hpp:41`, `rendering/RenderThread.hpp:46` |
| L10 | `GpuContracts.hpp` mixes CPU push-constant structs with `VkDeviceAddress`, included widely, couples gameplay-adjacent code to Vulkan headers | `rendering/GpuContracts.hpp:6` includes `volk.hpp` |

### Design goals

1. **One real abstraction seam.** Engine/render/pass code must compile **without** including `<vulkan/*>` or `volk.hpp`. Only `src/engine/vulkan/` and the new RHI `.cpp` files see Vulkan.
2. **Modern, declarative rendering.** Passes declare resources & state; the graph computes barriers/layouts. No hand-written `VkImageMemoryBarrier2` outside the RHI.
3. **Less bug-prone.** Kill mutable global singletons and `static bool` feature flags. Replace 11-arg wiring with a single typed context struct. Make resource handles type-safe and validity-checked.
4. **Preserve invariants** from `docs/ARCHITECTURE.md` §14 (esp. #1 WaitIdle-before-destroy, #6 VMA outlives allocations, #8 no `<vulkan/*>` leaks — which we are *tightening*, not loosening).

---

## 1. Target Architecture

```
┌─────────────────────────────────────────────────────────────┐
│ Engine / Rendering / Passes  (NO <vulkan/*>, NO volk.hpp)     │
│   gpu::CommandList, gpu::RenderGraph, gpu::TextureHandle,     │
│   gpu::BufferHandle, gpu::Format, gpu::PipelineHandle         │
├─────────────────────────────────────────────────────────────┤
│ RHI seam  (src/engine/gpu/  — headers Vulkan-free)            │
│   Opaque handles + Device interface. .cpp only sees Vulkan.   │
├─────────────────────────────────────────────────────────────┤
│ Vulkan backend  (src/engine/vulkan/  — the ONLY Vulkan code)  │
│   VulkanContext, GraphicsDevice, UniqueImage/Buffer, GpuHeap  │
└─────────────────────────────────────────────────────────────┘
```

### Core new types (all in `src/engine/gpu/`, Vulkan-free headers)

- `gpu::Format` — **complete** format enum (superset of current `GpuFormat`; add every format the engine actually uses). A single `ToVkFormat()/FromVkFormat()` lives in **one** `.cpp` in the vulkan backend.
- `gpu::TextureHandle`, `gpu::BufferHandle`, `gpu::PipelineHandle`, `gpu::SamplerHandle` — opaque, type-safe handles (struct wrapping a `uint32_t` generation+index, with `IsValid()`). Replaces raw `VkImage`/`VkBuffer`/`VkPipeline`/`RGImage` at the engine boundary.
- `gpu::CommandList` — Vulkan-free replacement for `rendering/CommandRecorder`. Methods take engine handles, not `Vk*`. Internally (in its `.cpp`) resolves to `VkCommandBuffer`.
- `gpu::ResourceState` enum (`Undefined`, `ColorAttachment`, `DepthAttachment`, `ShaderRead`, `StorageRead`, `StorageWrite`, `Present`, `TransferSrc`, `TransferDst`) — declarative; the graph maps state→layout+stage+access. No hand-written barriers in passes.
- `gpu::FrameContext` — single struct passed to wiring/passes, replacing 8–11 arg signatures.

---

## 2. Phased Execution

### Phase 1 — Complete the format/enum vocabulary (foundation, low risk)

**Goal:** make `gpu::Format` rich enough that no engine code needs `VkFormat`.
**Why:** L3 is the root cause of most leaks — passes reach for `VkFormat` only because `GpuFormat` is too thin.

**Files:**
- `src/engine/gpu/GpuTypes.hpp` (extend)
- NEW `src/engine/gpu/GpuEnums.hpp` (Vulkan-free: `Format`, `ResourceState`, `LoadOp`, `StoreOp`, `ImageUsage` bitflags, `ShaderStage` bitflags, `CompareOp`)
- NEW `src/engine/vulkan/GpuEnumConversions.{hpp,cpp}` — the ONLY translation unit mapping `gpu::*` ↔ `Vk*`. `.hpp` includes volk; lives under `vulkan/` so the leak rule holds.

**Steps:**
1. Enumerate every `VkFormat` currently referenced in `src/engine/{rendering,passes,material,text,ui}`. Use: `rg -n "VK_FORMAT_" src/engine | rg -v "src/engine/vulkan/"`.
2. Add a matching `gpu::Format` entry for each. Keep existing names (`R16G16B16A16Sfloat`, etc.) for source compatibility.
3. Implement `ToVk(gpu::Format)` / `FromVk(VkFormat)` and equivalents for the other enums in `GpuEnumConversions.cpp`. Add a `static_assert`-backed round-trip test helper.
4. Do **not** change any call sites yet.

**Acceptance:** builds on `vs2022-msvc` and `clangd` presets. `GpuEnumConversions.cpp` is the only new file including volk.
**Risk:** Low. Pure addition.

---

### Phase 2 — Introduce opaque handles + a Device-owned resource table

**Goal:** replace raw `VkImage/VkImageView/VkBuffer/VkPipeline` at the engine seam with `gpu::TextureHandle` etc.
**Why:** L1, L2, L6. Handles enable validity checks, generation counters (use-after-free detection), and central lifetime tracking → fixes the deferred-destruction correctness concerns and removes the need for `RGImage`'s ad-hoc id scheme.

**Files:**
- NEW `src/engine/gpu/GpuHandles.hpp` — `TextureHandle`, `BufferHandle`, `PipelineHandle`, `SamplerHandle` (each `{uint32_t index; uint32_t generation;}` + `IsValid()`, `operator==`).
- `src/engine/gpu/GpuDevice.{hpp,cpp}` — add a resource registry. `GpuDevice` already owns `GraphicsDevice*` and is the natural home.
  - `TextureHandle RegisterTexture(...)`, `BufferHandle RegisterBuffer(...)`, resolve methods (backend-only), `Destroy(handle)` that enqueues deferred destruction respecting `kMaxFramesInFlight`.
- `src/engine/vulkan/` — backend resolve table maps handle → `VkImage/VkImageView/VmaAllocation`.

**Steps:**
1. Define handles. Generation counter increments on slot reuse; `IsValid()` checks index in range AND generation matches → catches stale-handle bugs (replaces silent `0xFFFFFFFF` sentinels at `RenderGraph.hpp:25`, `RenderTargetService` etc.).
2. Move the **deferred-destruction** logic (`RenderGraph.hpp:357-369` `PendingDestruction`, `m_pendingDestructions[kMaxFramesInFlight]`) UP into `GpuDevice`/backend so all resources share one correct WaitIdle-respecting reclaim path (Invariant #1).
3. Keep `RGImage` temporarily as a thin alias over `TextureHandle` so `RenderGraph` keeps compiling. Mark `// TODO(phase3): remove`.

**Acceptance:** builds; existing render path unchanged at runtime (handles wrap the same Vk objects). Add a debug assert that firing `Resolve()` on an invalid handle logs `AE_ERROR(LogCategory::Vulkan, ...)`.
**Risk:** Medium. Lifetime bugs if deferred destruction migration is sloppy — do it behind the same 3-frame ring already proven in `RenderGraph`.

---

### Phase 3 — `gpu::CommandList` replacing raw `CommandRecorder`

**Goal:** passes record draws via a Vulkan-free command list.
**Why:** L1, L5. This is the highest-leverage change for "rendering cleaner / less bug-prone."

**Files:**
- NEW `src/engine/gpu/CommandList.{hpp,cpp}` (header Vulkan-free; `.cpp` includes volk).
- `src/engine/rendering/CommandRecorder.{hpp,cpp}` → becomes a thin backend detail OR is absorbed into `CommandList.cpp`.
- `src/engine/passes/*.{hpp,cpp}` — migrate pass callbacks.

**API sketch (Vulkan-free):**
```cpp
namespace aether::gpu {
class CommandList {
public:
    void BindPipeline(PipelineHandle);
    void BindIndexBuffer(BufferHandle, std::uint64_t offset = 0, IndexType = IndexType::U32);
    void PushConstants(PipelineHandle, ShaderStage, std::uint32_t offset, std::span<const std::byte>);
    void Draw(std::uint32_t vtx, std::uint32_t inst = 1, std::uint32_t firstVtx = 0, std::uint32_t firstInst = 0);
    void DrawIndexedIndirectCount(BufferHandle cmds, std::uint64_t cmdOffset,
                                  BufferHandle count, std::uint64_t countOffset,
                                  std::uint32_t maxDraws);
    void BeginDebugLabel(std::string_view, Color = {});
    void EndDebugLabel();
    // NO MemoryBarrier2 here — barriers belong to the RenderGraph (Phase 5).
};
}
```

**Steps:**
1. Implement `CommandList` over `VkCommandBuffer` in `.cpp` (move existing `CommandRecorder.cpp` bodies).
2. Migrate `ForwardPass`, `SkyboxPass`, `CullPass`, `PostProcessStack` to take `gpu::CommandList&` and engine handles. Replace `std::function<void(VkCommandBuffer, VkPipelineLayout)>` (L5) with `std::function<void(gpu::CommandList&, PipelineHandle)>` or a small `LightingBindContext`.
3. Bindless descriptor set binding becomes `cmd.BindBindlessSet()` (the bindless set is engine-global; hide the `VkDescriptorSet`).
4. Remove the public debug-fn statics (`CommandRecorder.hpp:57-59`) from the engine header; keep them as backend internals.

**Acceptance:** no pass `.hpp` under `src/engine/passes/` includes `volk.hpp`/`<vulkan/*>`. Frame renders identically. Verify with `rg -l "volk.hpp|vulkan/" src/engine/passes` returning nothing.
**Risk:** Medium-high (touches hot draw path). Migrate one pass at a time; keep old recorder until all passes move.

---

### Phase 4 — Replace 8–11 arg wiring with `gpu::FrameContext`

**Goal:** kill parameter-explosion (L8).
**Why:** `RenderTargetService::BindRuntime` (11 args, 7 raw ptrs) and `ForwardPass::RegisterPass` (8 args) are error-prone and order-sensitive.

**Files:** NEW `src/engine/rendering/FrameContext.hpp`; edit `RenderTargetService.{hpp,cpp}`, all `passes/*`.

**Steps:**
1. Define one struct holding the stable per-frame/runtime references:
```cpp
struct FrameContext {
    RenderGraph*      graph        = nullptr;
    BindlessManager*  bindless     = nullptr;
    CameraManager*    cameras      = nullptr;
    LightingManager*  lighting     = nullptr;
    Renderer*         renderer     = nullptr;
    MaterialBuffer*   materials    = nullptr;
    const CullPass*   cullPass     = nullptr;
    std::function<std::uint64_t()> frameIndex;
    gpu::Format       depthFormat  = gpu::Format::Undefined;
    gpu::Format       colorFormat  = gpu::Format::Undefined;
};
```
2. Change `BindRuntime(...)` → `BindRuntime(const FrameContext&)`. Same for pass `RegisterPass`.
3. Add `AE_ASSERT` in `BindRuntime` that all required pointers are non-null (currently a silent nullptr deref hazard).

**Acceptance:** builds; `BindRuntime` and pass registration each take ≤2 params. No behavior change.
**Risk:** Low. Mechanical.

---

### Phase 5 — Declarative barriers: pass declares `ResourceState`, graph computes transitions

**Goal:** remove hand-written barrier/layout tracking from the engine seam (L7); make the graph the single source of truth.
**Why:** Manual `oldLayout/newLayout/srcStage/dstStage` is the classic Vulkan bug source.

**Files:** `rendering/RenderGraph.{hpp,cpp}` (port `Vk*` in public API to `gpu::*`), `passes/*`.

**Steps:**
1. Public `RenderGraph` API switches from `VkFormat/VkImageUsageFlags/VkImageLayout/VkAttachmentLoadOp/VkClearValue` to `gpu::Format/ImageUsage/ResourceState/LoadOp/ClearValue`. `PassContext` exposes `gpu::CommandList&` not `CommandRecorder&`; drop `VkExtent2D` for `gpu::Extent2D`.
2. `FrameTarget` becomes handle-based (`TextureHandle colorTarget/depthTarget`) instead of raw `VkImage/VkImageView/VkFormat`.
3. Keep the existing barrier *solver* (`Compile()`, `CompiledBarrier`, `ResourceState`) but move it **inside** the backend `.cpp`; it consumes declared `gpu::ResourceState` transitions and emits `VkImageMemoryBarrier2`. Passes never see barriers.
4. Validation pass: in debug, assert every resource read by a pass was written/declared by a prior pass or is external (catches missing-barrier classes of bugs at graph-compile time, logged via `LogCategory::Render`).

**Acceptance:** `rendering/RenderGraph.hpp` includes no `<vulkan/*>`/volk. Frame renders identically. RenderDoc capture shows the same barriers as before.
**Risk:** High (correctness of barriers). Validate with validation layers ON and a RenderDoc diff before/after.

---

### Phase 6 — Eliminate mutable global singletons & static feature flags

**Goal:** remove L6, L9.
**Why:** `RenderGraph::s_current` (public, mutable) and `static bool s_enabled` are hidden global state shared across the engine + render thread (Invariant #3: render thread isolation).

**Files:** `rendering/RenderGraph.{hpp,cpp}`, `passes/ForwardPass.{hpp,cpp}`, `rendering/RenderThread.{hpp,cpp}`.

**Steps:**
1. Remove `static RenderGraph* s_current` + `GetCurrentRenderGraph()`. Pass the graph explicitly (it's already in `FrameContext`/`PassContext`). Grep callers: `rg -n "GetCurrentRenderGraph|s_current"`.
2. Move `ForwardPass::s_enabled` and similar feature toggles into a `RenderFeatureFlags` struct owned by `RenderingSubsystem` (instance state, set from `EngineSettings`). Pass through `FrameContext`.
3. `RenderThread::IsReloadInProgress()` static → an atomic owned by the `RenderThread` instance, queried via the instance (the thread already has an owner).

**Acceptance:** `rg -n "static.*s_(enabled|current)" src/engine/{rendering,passes}` returns nothing. No data races flagged by TSan-equivalent reasoning; render thread reads flags from the immutable `RenderFramePacket`/context snapshot only.
**Risk:** Medium. Ensure flags consumed on the render thread come from the per-frame snapshot, not live engine state.

---

### Phase 7 — Decouple `GpuContracts.hpp` from Vulkan headers

**Goal:** remove L10.
**Why:** `GpuContracts.hpp` includes `volk.hpp` only for `VkDeviceAddress` (== `uint64_t`). Widely included → drags Vulkan into many TUs.

**Steps:**
1. Replace `VkDeviceAddress` with `gpu::DeviceAddress` (a `using DeviceAddress = std::uint64_t;` in `gpu/GpuTypes.hpp`).
2. Drop `#include "vulkan/volk.hpp"` from `GpuContracts.hpp`. Keep every `static_assert(sizeof/offsetof ...)` — they must still pass (layout unchanged, `uint64_t` is ABI-identical to `VkDeviceAddress`).

**Acceptance:** `GpuContracts.hpp` includes no Vulkan header; all existing `static_assert`s pass; shader contract sync comments still hold.
**Risk:** Low.

---

### Phase 8 — Enforcement & guardrails (prevent regression)

**Goal:** make the abstraction self-enforcing so future code can't re-leak.

**Steps:**
1. Add a CI/check script (or a clang-tidy custom check / simple `rg` gate) asserting: no file under `src/engine/{rendering,passes,scene,assets,material,camera,animation,ui,text}` includes `<vulkan/`, `volk.hpp`, or `vk_mem_alloc.h`. Allowlist: `src/engine/gpu/*.cpp` and `src/engine/vulkan/**`.
2. Document the seam in `docs/ARCHITECTURE.md` (update §"GPU/Vulkan two-layer") and add Invariant #11: *"Only `src/engine/vulkan/` and `src/engine/gpu/*.cpp` may include Vulkan headers."*
3. Re-run `graphify update .` to refresh the knowledge graph after the refactor.

**Acceptance:** the grep gate passes; `docs/ARCHITECTURE.md` updated.
**Risk:** None functional.

---

## 3. Execution Rules for the Agent

- **One phase per PR/commit.** Each phase must compile (`cmake --preset vs2022-msvc && cmake --build --preset vs2022-msvc --config Debug`) and the `clangd` preset must regenerate cleanly.
- After adding/removing/renaming any `.cpp/.hpp` or editing `CMakeLists.txt`, run `/sync-lsp`.
- **Never** reorder subsystem init/shutdown in `AetherCore.cpp` (Invariant #2).
- **Never** commit `.spv` files. **Never** touch `build*/` or `build/_deps/`.
- Always `m_gpu->WaitIdle()` before destroying GPU resources (Invariant #1); the Phase-2 deferred-destruction migration must preserve this.
- Follow `AGENTS.md` naming: `gpu::` namespace snake_case, types PascalCase, members `m_snake_case`, constants `kPascalCase`. Tabs for indent. `#pragma once`. `SortIncludes: Never`.
- Use `Expected<T>` + `AE_TRY` for fallible RHI calls; `AE_ASSERT` for programmer errors.
- Validate barrier-affecting phases (3, 5) with Vulkan validation layers ON and a RenderDoc before/after comparison; the rendered frame must be pixel-identical.

## 4. Suggested Order & Dependencies

```
P1 (enums)  ──▶ P2 (handles) ──▶ P3 (CommandList) ──▶ P5 (declarative barriers)
   │                                  │
   └──▶ P7 (contracts decouple)       └──▶ P4 (FrameContext) ──▶ P6 (kill globals)
                                                                      │
                                          P8 (enforcement) ◀──────────┘
```
P1 and P7 can land first (low risk, unblock everything). P3 before P5. P8 last.

## 5. Phase Progress (audited 2026-06-12)

| Phase | Status | Notes |
|---|---|---|
| P1 Format enum | ✅ done | `gpu::Format` + `vulkan/GpuEnumConversions.cpp` |
| P2a Opaque handles | ✅ done | `gpu::GpuHandles.hpp` |
| P2b ResourceRegistry (consolidation) | ✅ done | `gpu/ResourceRegistry.cpp` is a 30-line forwarding bridge (`CreateBuffer`/`CreateMappedBuffer`/`CreateTexture` → `s_reg`). `vulkan/ResourceRegistry.cpp` owns `Init(VkDevice, VmaAllocator)` + VMA allocation. `s_mappedSlots` removed. |
| P2b ResourceRegistry (UniqueBuffer/Image replacement) | ❌ not started | Long-term Phase B. CreateTexture stub is now fully implemented with VMA calls. CreateBuffer, CreateMappedBuffer, CreatePipeline, CreatePipelineLayout all exist in `gpu::ResourceRegistry`. Phase A consolidation is done. Replace `UniqueBuffer`/`UniqueImage` members engine-wide with `BufferHandle`/`TextureHandle`. 15-step order in `docs/plans/resource-registry-consolidation.md §3`. |
| P3 CommandList | ✅ done | **CommandRecorder fully deleted** — no `CommandRecorder.{hpp,cpp}` files remain. `PassContext::recorder` is `gpu::CommandList&`. `AetherCore.cpp` uses `m_currentCmdList = m_gpu->GetCurrentCommandList()`. All passes + RenderQueue + LightingManager + AsyncComputeContext migrated. |
| P4 FrameContext | ✅ done | `src/engine/rendering/FrameContext.hpp`. `RenderTargetService::BindRuntime`, `ForwardPass::RegisterPass`, `PassRegistrationContext` all consume the struct. `AE_ASSERT` guards in `BindRuntime`. |
| P6 Kill global singletons | ✅ done | `RenderGraph::s_current`, `ForwardPass::s_enabled`, `RenderThread::s_reloadInProgress` all gone. `rg -n "static.*s_(enabled|current)" src/engine/{rendering,passes}` returns nothing. `RenderThread` in `ServiceContainer`. |
| P7 GpuContracts decouple | ✅ done | `rendering/GpuContracts.hpp` includes no Vulkan header; uses `gpu::DeviceAddress`. All `static_assert` checks intact. |
| P5 Declarative barriers / handle APIs | ⚠️ **partially done** | Surface migrations done: PassBuilder API is `gpu::*`; `BindlessManager::GetLayout/GetSet` → `gpu::DescriptorSetLayout/DescriptorSet`; `Swapchain::GetImageFormat/GetDepthFormat` → `gpu::Format`; AsyncComputeContext → `gpu::CommandList`; pass headers clean. **Remaining (see §7):** RenderGraph internal storage + `FrameTarget` still raw Vulkan (sub-scope d); GraphicsPipeline + CullPass compute-pipeline creation still call `vk*` directly (c/a); 25 engine headers still drag `volk.hpp` (b + others). |
| P8 Enforcement gate | ❌ not started | Last. CI/pre-submit grep that locks the abstraction in + Invariant #11. |

**App layer is fully clean** as of this audit: `rg "volk.hpp\|#include <vulkan/\|static_cast<Vk" src/app` returns nothing. The §7.1 app-residual table (the 10 files / 25 leak lines flagged 2026-06-11) has been entirely resolved.

## 6. Anti-Patterns (learned this session)

- **AP-1**: Don't fix leaks ad-hoc. The `LightingManager` narrow-scope fix was Phase 5 work slipped into P3. Result: 5 `static_cast` cruft sites in `app/`. A `static_cast` is acceptable as transitional shim ONLY when (a) the API change is part of the phase AND (b) the cast is removed before the phase ends.
- **AP-2**: Don't start Phase 5 without finishing P3 + P4. P5 cannot start until both P3 and P4 are complete. The narrow leak fix exception is the only acceptable overlap.
- **AP-3**: Don't pre-migrate API consumers. When changing a public API, migrate all consumers in the same slice. Use `// TODO(phase5): remove static_cast`.
- **AP-4**: Don't trust MSVC-only as ground truth. Any phase must build clean on BOTH compilers: `cmake --build --preset vs2022-msvc --config Debug` AND `cmake --build D:\AetherCore\build-ninja-clang`.
- **AP-5**: One P5 sub-scope per slice, never combine. The sub-scopes in §7 are sized so each lands as a single commit with a clean build on both compilers. Don't "do (a)+(b) while I'm here" — (a) is LOW-MEDIUM risk and (d) is HIGH risk, so a combined slice is impossible to bisect. §7.9 is a DAG, not a linear order; independent sub-scopes can land in parallel sessions. Pick ONE, land it, update §5, move on.

## 7. Remaining Vulkan Leak Surface — P5 sub-scope inventory (audited 2026-06-12)

**Why this section exists:** P3 + P4 + P6 + P7 are done, plus the (e)/(f)/(g) surface swaps and all pass-header decouplings. The app layer is clean. What remains is the deeper P5 work: RenderGraph internal storage, the pipeline-creation factory split, and the 25 engine headers that still drag `volk.hpp`. This section enumerates the remaining surface and slices it into small, mechanical sub-scopes.

### 7.1 Header leak surface (audit 2026-06-12)

**25 non-vulkan, non-gpu engine headers still `#include "vulkan/volk.hpp"`** (down from 31; pass headers, `GraphicsPipeline.hpp`, `BindlessManager.hpp`, `AsyncComputeContext.hpp`, `AnimationRootMotion.hpp`, `MeshUploadQueue.hpp`, and the now-deleted `CommandRecorder.hpp` are all clean):

| Subsystem | Headers |
|---|---|
| `animation/` | `AnimationCompiler.hpp`, `AnimationDatabase.hpp` |
| `assets/` | `AssetSubsystem.hpp` |
| `material/` | `BindlessContract.hpp`, `MaterialBuffer.hpp`, `Texture.hpp` |
| `mesh/` | `Mesh.hpp`, `MeshArena.hpp`, `PrimitiveMeshes.hpp` |
| `passes/` | `PostProcessStack.hpp` (only: `VkFormat` field) |
| `rendering/` | `GpuTimestampPool.hpp`, `Renderer.hpp`, `RenderGraph.hpp`, `RenderPipelineCoordinator.hpp`, `RenderQueue.hpp`, `RenderTargetService.hpp`, `ShadowAtlasManager.hpp`, `ShadowService.hpp` |
| `scene/` | `Components.hpp` |
| `text/` | `FontAtlas.hpp` |
| `ui/` | `QuadRenderer.hpp`, `UiLayout.hpp`, `UiSystem.hpp`, `UiWidgets.hpp` |
| `utils/` | `GpuProfiler.hpp` |

**App layer: fully clean.** `rg "volk.hpp|#include <vulkan/|static_cast<Vk" src/app` returns nothing. The entire 2026-06-11 app-residual table is resolved (no `static_cast<VkDescriptorSetLayout>`, no `static_cast<VkExtent2D>`, no `GetCurrentCommandRecorder`).

**Architectural `static_cast<Vk*>` outside `gpu/` and `vulkan/` (18 sites, the real leaks):**

| File | Sites | Cause | Sub-scope |
|---|---|---|---|
| `rendering/GraphicsPipeline.cpp` | 9 | pipeline create/destroy via raw `vk*` | (c) factory split |
| `passes/CullPass.cpp` | 5 | compute pipeline + layout creation via raw `vk*` | (c)/(a) |
| `rendering/LightingManager.cpp` | 1 | `static_cast<VkDescriptorSetLayout>(m_setLayout)` for pipeline create | (c) |
| `rendering/RenderQueue.cpp` | 1 | `GetCommandBuffer()` → `VkCommandBuffer` for direct `vkCmd*` | (b)/CmdList gap |
| `rendering/RenderGraph.cpp` | 1 | `GetCommandBuffer()` → `VkCommandBuffer` in barrier solver | (d) |
| `engine/AetherCore.cpp` | 1 | `GetCommandBuffer()` for Tracy `AE_PROFILE_GPU_ZONE_T` | acceptable (profiling) |

(Note: `static_cast<VkDeviceSize>(...)` for size arithmetic is widespread and benign — it forces `volk.hpp` in the TU but is not an abstraction leak. Those disappear naturally when the owning class moves off `UniqueBuffer` to `BufferHandle` in P2b Phase B.)

### 7.2 P5 sub-scope (a) — Passes surface: kill raw `vkCmd*`/`vkCreate*` in pass bodies

**Status: ✅ DONE (2026-06-12).** All pass headers and bodies are clean. `CullPass.hpp`, `ForwardPass.hpp`, `SkyboxPass.hpp`, `ForwardPass.cpp`, and `PostProcessStack.cpp` have zero architectural leaks. The previously remaining `CullPass.cpp` compute-pipeline creation (5 `static_cast<Vk*>` sites) was resolved by sub-scope (c) (factory split). `SkyboxPass.cpp` had `volk.hpp` removed and `PostProcessStack`'s format API was fully migrated to `gpu::Format` as part of sub-scope (f).

**Why (original):** pass headers used to include `volk.hpp` only to return `VkPipeline`/`VkPipelineLayout` from getters. That is resolved.

**Files (estimated 8):**
- `src/engine/passes/CullPass.hpp` — drop `VkPipeline`/`VkPipelineLayout` getters, replace with `gpu::Pipeline` / `gpu::PipelineLayout` (or, preferred, accept them in the `RegisterPass` callback and let `RenderGraph` resolve them — eliminates the getter API entirely).
- `src/engine/passes/ForwardPass.hpp` — drop `VkPipelineLayout` from `pushLightingFn` signature; replace with `gpu::PipelineLayout`.
- `src/engine/passes/PostProcessStack.hpp` — `CreateDesc` struct: `VkDevice`→`gpu::Device`, `VkPipelineCache`→`gpu::PipelineCache`, `VkExtent2D`→`gpu::Extent2D`, `VkFormat`→`gpu::Format`.
- `src/engine/passes/SkyboxPass.hpp` — same struct field swap.
- `src/engine/passes/PostProcessStack.cpp`, `physics/PhysicsDebugRenderer.cpp`, `ui/QuadRenderer.cpp`, `rendering/LocalShadowService.cpp` — every `static_cast<VkCommandBuffer>(ctx.recorder.GetCommandBuffer())` site (~25 sites) gets replaced by a `gpu::CommandList` method call. Most are `vkCmdSetViewport` / `vkCmdSetScissor` / `vkCmdBindPipeline` / `vkCmdDrawIndirect` / `vkCmdPipelineBarrier2` — all have existing `gpu::CommandList` overloads (see `gpu/CommandList.cpp:312,337,347,369`).

**Acceptance:**
- `rg -l "volk.hpp|vulkan/" src/engine/passes` returns nothing.
- `rg -l "static_cast<VkCommandBuffer>" src/engine/{passes,rendering,ui,physics}` returns nothing (or only `// TODO(phase5g): remove static_cast` marked sites in `AsyncComputeContext`).
- Builds clean on both compilers.

**Risk:** LOW-MEDIUM. Mechanical migration; the only risk is missing an `Execute()` callback method on `gpu::CommandList`. If a `vkCmd*` is needed and missing, add it to `gpu/CommandList` (header + impl).

### 7.3 P5 sub-scope (b) — Texture / Mesh / Font / Material / AssetSubsystem: drop volk from headers

**Why:** `Texture.hpp`, `Mesh.hpp`, `MeshArena.hpp`, `MeshUploadQueue.hpp`, `PrimitiveMeshes.hpp`, `FontAtlas.hpp`, `MaterialBuffer.hpp`, `BindlessContract.hpp`, `AssetSubsystem.hpp` all include `volk.hpp` because they expose `VkImage`/`VkImageView`/`VkDeviceMemory`/`VmaAllocation` in their public types or factory return values.

#### 7.3.0 Architectural rule (added 2026-06-11 after the (b1) revert)

> **Engine `.cpp` files in `material/`, `mesh/`, `text/`, `assets/`, `animation/`, `passes/`, `rendering/`, `ui/`, `physics/`, `scene/` may not call any `vk*` function directly.** They may only call `gpu::*` helpers. Every `vk*` call site in those folders must be hidden behind a `gpu::` factory whose impl lives in `src/engine/vulkan/`.

The earlier (b1) attempt to drop `volk.hpp` from these headers while leaving `static_cast<VkDevice>` / `static_cast<VkCommandPool>` in their `.cpp` bodies was rejected as a half-fix: it relocates the leak rather than removing it. The new rule is: a `static_cast<VkX>` outside `src/engine/vulkan/` is itself a leak.

**Files (estimated 6):**
- `src/engine/material/Texture.hpp` — `UniqueImage` getter returns `VkImage` (replace with `gpu::ImageHandle`), `GetDefaultView()` returns `VkImageView` (replace with `gpu::ImageView`). `VkDescriptorSet GetDescriptorSet()` → `gpu::DescriptorSet`.
- `src/engine/mesh/Mesh.hpp` — `BeginOneTimeBuffer(VkDevice, ...)` factory → take `gpu::Device` instead.
- `src/engine/mesh/MeshArena.hpp`, `MeshUploadQueue.hpp`, `PrimitiveMeshes.hpp` — same `VkBuffer`/`VmaAllocation` exposure.
- `src/engine/text/FontAtlas.hpp` — `VkImage` field on font glyphs.
- `src/engine/material/MaterialBuffer.hpp` — `BindlessContract` uses raw `VkDescriptorSetLayoutBinding` (replace with `gpu::DescriptorSetLayoutBinding`, already defined in `gpu/GpuTypes.hpp:92-98`).
- `src/engine/material/BindlessContract.hpp` — same.
- `src/engine/assets/AssetSubsystem.hpp` — `VkCommandPoolCreateInfo` (replace with `gpu::CommandPoolDesc` or accept an opaque handle).

**Prerequisite — P2b ResourceRegistry consolidation (supersedes the old b0 factory layer):**

> **2026-06-11 design update:** The original `b0` factory layer (`gpu::CreateMappedBuffer`, `gpu::CreateUploadContext`, `gpu::CreateBindlessPipelineLayout` as separate free functions) has been **superseded** by the ResourceRegistry consolidation plan. Once `aether::ResourceRegistry` gains `CreateBuffer`/`CreateMappedBuffer`/`CreateTexture` factory methods (P2b consolidation, `docs/plans/resource-registry-consolidation.md §2`), those methods become the `b0` factory layer. Do **not** create separate `gpu/MappedBuffer.{hpp,cpp}` or `gpu/UploadContext.{hpp,cpp}` files — the registry is the right home.
>
> The one exception is `gpu::CreateUploadContext` / `gpu::DestroyUploadContext` for `AssetSubsystem` — the upload context pattern (command pool + command buffer + fence + one-time submit) is genuinely separate from buffer/texture lifetime. This remains a standalone `gpu/UploadContext.{hpp,cpp}` backed by `vulkan/UploadContext.cpp`. It does not need to be part of the registry.

| New API | Replaces | Status |
|---|---|---|
| `gpu::ResourceRegistry::CreateBuffer(desc)` | `UniqueBuffer::CreateDeviceLocal(...)` call sites | ✅ exists (`vulkan/ResourceRegistry.cpp`); call sites not yet migrated |
| `gpu::ResourceRegistry::CreateMappedBuffer(desc)` | `UniqueBuffer::CreateMapped(...)` call sites | ✅ exists; call sites not yet migrated |
| `gpu::ResourceRegistry::CreateTexture(desc)` | `UniqueImage::Create(...)` call sites | ⚠️ **stub only** (`vulkan/ResourceRegistry.cpp` ignores `desc`); needs real impl before UniqueImage replacement |
| `gpu::CreateUploadContext(device, family)` | 10 raw `vk*` calls in `AssetSubsystem::Init` | ❌ not started — standalone `gpu/UploadContext.{hpp,cpp}` |
| `gpu::CreateBindlessPipelineLayout(desc)` | `BindlessContract::CreatePipelineLayoutWithBindless` | Part of P5(e) BindlessManager API migration; `BindlessContract.hpp` keeps only constants |

The rule remains: mechanical swaps without proper factories just relocate Vulkan cruft via `static_cast`. The audit `rg "static_cast<Vk" src/engine/{material,mesh,text,assets}` must return nothing after P5(b) is complete.

**Acceptance:**
- `rg -l "volk.hpp" src/engine/{material,mesh,text,assets}` returns nothing.
- `rg "static_cast<Vk" src/engine/{material,mesh,text,assets,animation,passes,rendering,ui,physics,scene}` returns nothing.
- `rg "vk[A-Z][a-zA-Z]+\(" src/engine/{material,mesh,text,assets,animation,passes,rendering,ui,physics,scene}` returns nothing.

**Risk:** MEDIUM. AssetSubsystem has 1-2 hot-path call sites that need to update. With the factory layer prerequisite, the (b) header decouplings become mechanical `gpu::X`→engine-type swaps.

### 7.4 P5 sub-scope (c) — pipeline-creation backend split (`GraphicsPipeline` + compute) ✅ DONE (2026-06-12)

**Status: ✅ DONE (2026-06-12).** The full graphics-pipeline factory body was extracted to `src/engine/vulkan/GraphicsPipelineFactory.cpp` and the compute-pipeline factory body to `src/engine/vulkan/ComputePipelineFactory.cpp`. `GraphicsPipeline.cpp` is now a thin forwarder to the Vulkan factory via `gpu::ResourceRegistry`. `CullPass.cpp` routes all compute-pipeline creation through `gpu::ResourceRegistry::CreateComputePipeline`. All architectural `static_cast<Vk*>` sites in both `GraphicsPipeline.cpp` and `CullPass.cpp` are eliminated.

**Acceptance (verified):**
- `rg "vk(Create|Destroy)(Graphics|Compute)Pipelines?|vkCreatePipelineLayout" src/engine/{rendering,passes}` returns nothing.
- `rg "static_cast<Vk(Pipeline|PipelineLayout|Device|DescriptorSetLayout)>" src/engine/{rendering,passes}` returns nothing.

**Risk:** N/A — complete.

### 7.5 P5 sub-scope (d) — `RenderGraph` internal storage Vk→gpu + handle-based `FrameTarget`

**Status (2026-06-12): NOT started — the single largest remaining leak and the only HIGH-risk one.** Public PassBuilder/`Execute` surface is already `gpu::*` (the `CommandRecorder` `Execute` overload is gone; only `Execute(gpu::CommandList&, ...)` remains at `RenderGraph.hpp:194`). Everything below the surface is still raw Vulkan.

**What remains (verified 2026-06-12):**
- `RenderGraph.hpp:9` `#include <vk_mem_alloc.h>`, `RenderGraph.hpp:10` `#include "vulkan/volk.hpp"`.
- Internal structs all raw `Vk*`: `AttachmentRef` (line 222), `PassRecord` (line 249), `CompiledBarrier` (line 261, uses `VkImageLayout`/`VkPipelineStageFlags2`/`VkAccessFlags2`), `ResourceState` (line 279). `m_scratchBarriers` is `std::vector<VkImageMemoryBarrier2>` (line 350).
- Public methods still leak: `RegisterImage(VkImage, VkImageView, ...)` (line 148), `EnsureBindlessSampled(... VkDevice, VkImageLayout)` (line 159), `ResolveImage() -> VkImage` (line 332), `ResolveView() -> VkImageView` (line 333).
- `RenderGraph.cpp:674` has the one architectural cast `static_cast<VkCommandBuffer>(cmd.GetCommandBuffer())` inside the barrier solver.
- `FrameTarget` is defined **in `RenderGraph.hpp` (lines 58–63)**, NOT in `GpuDevice.hpp` (which only forward-declares it at line 22 and exposes `BuildFrameTarget()` at line 58). Fields are raw `VkImage colorImage/depthImage`, `VkImageView colorView/depthView`, `VkFormat colorFormat/depthFormat`.

**Files (estimated 3):**
- `src/engine/rendering/RenderGraph.hpp` — drop `volk.hpp` + `vk_mem_alloc.h`. Move `AttachmentRef`/`CompiledBarrier`/`PassRecord`/`ResourceState` into a backend-only `vulkan/RenderGraphStorage.hpp`. Migrate `FrameTarget` fields and the public `RegisterImage`/`EnsureBindlessSampled`/`ResolveImage`/`ResolveView` signatures to `gpu::ImageHandle`/`gpu::ImageView`/`gpu::Format`/`gpu::ResourceState`.
- `src/engine/rendering/RenderGraph.cpp` — barrier solver consumes declared `gpu::ResourceState`, converts to `Vk*` at emit time via `vulkan/GpuEnumConversions.hpp`. The `static_cast<VkCommandBuffer>` at line 674 stays only if it moves into a backend TU.
- `src/engine/gpu/GpuDevice.{hpp,cpp}` — `BuildFrameTarget()` returns the handle-based `FrameTarget`. Check `AetherCore.cpp` swapchain-callback consumer.

**Acceptance:**
- `src/engine/rendering/RenderGraph.hpp` does not include `volk.hpp` or any `<vulkan/*>`.
- `rg "Vk" src/engine/rendering/RenderGraph.hpp` returns nothing.

**Risk:** HIGH (plan §5 explicitly calls this out as the highest-risk sub-scope; barrier solver correctness). We have no display — verify by code review of the barrier solver + Vulkan validation layers if a capture host is available.

### 7.6 P5 sub-scope (e) — `BindlessManager` API Vk→gpu ✅ DONE (2026-06-12)

`BindlessManager.hpp` is clean (no `volk.hpp`). `GetLayout()` returns `gpu::DescriptorSetLayout` (line 56), `GetSet()` returns `gpu::DescriptorSet` (line 57). `ForwardPass.hpp` `pushLightingFn` signature migrated. All 16 `static_cast<VkDescriptorSetLayout>` cruft sites in `app/systems/*` are gone — `rg "static_cast<VkDescriptorSetLayout>" src/app` returns nothing. The one remaining `static_cast<VkDescriptorSetLayout>` in the engine is `LightingManager.cpp:554` (passing the layout into pipeline create) — that belongs to sub-scope (c)'s factory split, not (e).

### 7.7 P5 sub-scope (f) — swapchain/pass format getters → `gpu::Format`

**Status (2026-06-12): ✅ mostly done (2026-06-12)** — `PostProcessStack::GetForwardColorFormat()` migrated to return `gpu::Format`; `ShadowService`/`LocalShadowService` `depthFormat` stored as `gpu::Format`; `ShadowAtlasManager::kAtlasFormat` is `gpu::Format`. `gpu::FromVk` call count reduced from 11 → 4 residual. `vulkan/Swapchain.hpp` returns `gpu::Format` from both getters.

**Remaining `gpu::FromVk(...)` residual (4 sites):**
- `rendering/RenderTargetService.cpp` — `OnRenderGraphReset(...)` still takes two `VkFormat` parameters (2 calls). This is the only engine-layer residual; it will be eliminated when `RenderTargetService` is migrated in sub-scope (b)/(d).
- `vulkan/Swapchain.cpp` (2 calls) — internal to the Vulkan backend; permitted by the abstraction rule.
- `vulkan/UniqueImage.cpp` (1 call) — internal to the Vulkan backend; permitted.

**Acceptance:**
- `rg "gpu::FromVk" src/engine/{rendering,passes}` returns nothing.

**Risk:** LOW. Mechanical.

### 7.8 P5 sub-scope (g) — `AsyncComputeContext` swap + drop `CommandRecorder` everywhere ✅ DONE (2026-06-12)

`CommandRecorder.{hpp,cpp}` is **fully deleted** — the files no longer exist; the only residual mention is a doc comment in `gpu/CommandList.hpp:22`. `rg "\bCommandRecorder\b" src/` finds nothing else. `AsyncComputeContext.hpp` is clean and `GetCommandList(uint32)` returns `gpu::CommandList` (line 31). `AetherCore.cpp` uses `m_currentCmdList = m_gpu->GetCurrentCommandList()` (line 247) and drives barriers/labels/`Execute` through it (lines 449, 456, 458, 459); `m_currentRecorder`/`GetCurrentCommandRecorder` are gone. `PassContext::recorder` is `gpu::CommandList&`.

### 7.9 P5 sub-scope dependency order (remaining work as of 2026-06-12)

DONE: (e) BindlessManager, (f) Swapchain getters (residual only), (g) AsyncComputeContext + CommandRecorder deletion. Remaining order:

```
(already completed: (a) passes bodies, (c) pipeline-creation factory split, (e) BindlessManager, (f) mostly, (g) CommandRecorder deletion)
```

**Rule (updated 2026-06-11):** The original `b0` factory layer (separate `gpu/MappedBuffer.hpp` etc.) is superseded by P2b ResourceRegistry consolidation. The P2b consolidation (`ResourceRegistry::CreateBuffer/CreateMappedBuffer/CreateTexture`) must land before P5(b) header decoupling. The (b1) attempt on 2026-06-11 was reverted because it used `static_cast<VkDevice>` etc. in `material/MaterialBuffer.cpp`, `assets/AssetSubsystem.cpp`, and `material/BindlessContract.cpp` bodies. The new rule (see §7.3.0): engine `.cpp` files may not call `vk*` and may not contain `static_cast<Vk*>` outside `vulkan/`.

**Remaining work as of 2026-06-12 (completed sub-scopes (a), (c), (e), (f) mostly, (g) removed from this diagram):**

```
P2b ResourceRegistry::CreateTexture (finish the stub) + gpu::UploadContext (standalone, AssetSubsystem only)
         │   (docs/plans/resource-registry-consolidation.md §2)
         ▼
(b) Texture/Mesh/Font/Material/AssetSubsystem headers drop volk
         │
         └──▶ (d) RenderGraph internal storage Vk→gpu + handle-based FrameTarget   [HIGH risk, last]

  (f) residual: RenderTargetService::OnRenderGraphReset VkFormat params → gpu::Format
                (independent; can land any time before (d))

  After (d): P2b Phase B — UniqueBuffer/UniqueImage replacement
  (docs/plans/resource-registry-consolidation.md §3)
  Migrate leaf classes first (MaterialBuffer → AnimationBlend/Ik → RenderQueue → ... → RenderGraph last)
```

### 7.10 ResourceRegistry consolidation (P2b design update — 2026-06-11)

The current two-class design (`aether::ResourceRegistry` in `vulkan/` + `aether::gpu::ResourceRegistry` static facade in `gpu/`) has a design smell: `gpu/ResourceRegistry.cpp` maintains a parallel `s_mappedSlots` vector alongside `s_vkDevice`/`s_vmaAllocator` statics that duplicate state already owned or derivable by the backend. The full design analysis and two-phase fix plan is in `docs/plans/resource-registry-consolidation.md`.

**Phase A — Concluded 2026-06-11. ✅ Done.**
- Added `mappedPtr` + `deviceAddress` fields to `BufferEntry` — eliminates `s_mappedSlots`
- Added `Init(VkDevice, VmaAllocator)` + `Create*`/`ResolveMapped`/`FlushMapped` to `aether::ResourceRegistry`
- Moved all VMA allocation logic from `gpu/ResourceRegistry.cpp` into `vulkan/ResourceRegistry.cpp`
- Rewrote `gpu/ResourceRegistry.cpp` as 30 lines of forwarding — one pointer, no state
- Zero call-site changes

**Phase B summary (UniqueBuffer/UniqueImage replacement, long-term):**
- Replace `UniqueBuffer`/`UniqueImage` members in 14 engine classes with `BufferHandle`/`TextureHandle`
- Eliminates the three independent deferred-destruction mechanisms
- Ordered leaf-first; `RenderGraph` and `ResourcePool` are last
- Full 15-step migration order and per-class notes in `docs/plans/resource-registry-consolidation.md §3`

**Cumulative acceptance (P5 fully done):**
- `rg -l "volk.hpp|vulkan/" src/engine/{rendering,passes,ui,physics,material,mesh,text,assets,animation,scene,utils,camera}` returns nothing outside `src/engine/gpu/GpuEnumConversions.{hpp,cpp}` (the bridge) and `src/engine/gpu/CommandList.{hpp,cpp}` (also bridge).
- `rg "static_cast<Vk" src/` returns nothing (or only `// TODO` sites inside the bridge).
- `rg "\bCommandRecorder\b" src/` returns nothing.
- `src/engine/rendering/RenderGraph.hpp` does not include `volk.hpp` or any `<vulkan/*>`.

**Risk profile per sub-scope:** (e)/(f) LOW — pure mechanical swap. (a)/(b) LOW-MEDIUM — many call sites but the `gpu::CommandList` API covers almost all of them. (c) MEDIUM — factory split is invasive but localized. (g) MEDIUM — touches the per-frame hot path. (d) HIGH — barrier solver correctness; validate with Vulkan validation layers + RenderDoc diff per AP-2.

## 8. Out of Scope (explicitly)

- Switching graphics APIs (no D3D12/Metal backend now — but the RHI seam this plan builds is the prerequisite if ever wanted).
- Rewriting shaders or the bindless model (`material/BindlessContract`).
- Changing the render-thread/`RenderFramePacket` double-buffering design (it's sound; Invariant #3).
- GpuHeap thread-safety (Invariant #4) — leave asset-loading-thread-only contract intact.
