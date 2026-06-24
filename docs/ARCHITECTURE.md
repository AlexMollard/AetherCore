# AetherCore Architecture

This document describes how AetherCore fits together. It is intended for contributors who need to understand the engine's design decisions and invariants before making changes.

> AetherCore is structured as a **subsystem orchestrator** sitting above a **two-layer GPU abstraction**, with a dedicated **render thread** that consumes per-frame packets produced by the engine thread.

---

## 1. Top-level shape

```
                    ┌───────────────────────────────────────────┐
                    │                  AetherCore               │
                    │  (orchestrator - owns all subsystems)     │
                    └────────────────────┬──────────────────────┘
                                         │ owns unique_ptr to
            ┌────────────────┬───────────┼────────────┬────────────────┐
            ▼                ▼           ▼            ▼                ▼
    PlatformSubsystem   GpuDevice    Rendering-   CameraSubsystem   SceneSubsystem
    (Window/Input)      (gpu/)       Subsystem     (Cameras +        (ECS World,
                       ┌─┴──┐        (rendering/)  Lighting)         Scene)
                  ┌────▼─┐ ┌─▼─────────┐
                  │ vulkan/ │ AsyncComputeContext (gpu/)
                  │  impl   │ (optional dedicated compute)
                  └────────┘
            ┌────────────────────┬────────────────────┐
            ▼                    ▼                    ▼
       AssetSubsystem       UISubsystem          Animation systems
       (assets/, io/)       (ui/)                (animation/)
       ┌────────────┐
       │ MaterialBuffer,
       │  MeshArena,
       │  AssetManager
       └────────────┘
```

`AetherCore` is a single `class` defined in `src/engine/AetherCore.hpp:23`. Its constructor (`src/engine/AetherCore.cpp:45`) builds every subsystem, registers it on a `ServiceContainer`, and wires cross-subsystem dependencies. Its destructor tears them down in the exact reverse order, with `m_gpu->WaitIdle()` first to ensure no GPU work is in flight.

### Subsystem init order (from `AetherCore.cpp:50`)

The order is significant - each step depends on services registered by earlier steps. **Do not reorder these without understanding the dependency chain.**

1. **Platform** - `PlatformSubsystem` opens the window and instantiates `Window` + `Input`.
2. **Graphics device** - `GpuDevice::Init` (creates instance, device, swapchain, VMA, bindless manager).
3. **Scene** - `SceneSubsystem` instantiates the ECS `World` and `Scene`.
4. **Assets** - `AssetSubsystem` builds `AssetManager`, `MeshArena`, `MaterialBuffer`, registers virtual file system mounts.
5. **Cameras** - `CameraSubsystem` registers `CameraManager` + `LightingManager`.
6. **Rendering** - `RenderingSubsystem` registers `Renderer`, `RenderQueue`, `RenderGraph`, all passes.
7. **UI** - `UISubsystem` (opt-in via `uiFontPath` config; registers `UIRenderer` + `UiContext` + `UiSystem`).
8. **Async compute** - `AsyncComputeContext::Init` (skipped if no dedicated compute queue).
9. **Animation systems** - `AnimationBlendSystem` and `AnimationRootMotionSystem` are constructed and registered.
10. **Swapchain recreation callback** - `GpuDevice::SetSwapchainRecreatedCallback` lets the rendering subsystem re-register its UI passes when the swapchain changes.

**Shutdown is the exact reverse**, preceded by `m_gpu->WaitIdle()`.

---

## 2. The service locator (`ServiceContainer`)

`src/engine/utils/ServiceContainer.hpp:21` provides a type-erased service locator:

```cpp
template<typename T> void Register(T& service);
template<typename T> void RegisterOwned(std::unique_ptr<T> service);
template<typename T> T& Get() const;          // asserts if not registered
template<typename T> T* TryGet() const;       // returns nullptr if not registered
template<typename T> bool Has() const;
```

Subsystems register themselves during `Init()`. The `RegisterOwned` overload keeps shared ownership inside the container, so subsystems are automatically destroyed when the container is cleared.

**Thread safety:** NOT thread-safe. All registrations happen on the engine thread during init. Reads after init are safe as long as no concurrent modifications occur. This invariant is critical - do not register/unregister services from a render thread or coroutine.

**Lookup pattern:** subsystems reach for sibling services through `m_services.Get<T>()` rather than direct pointers. The compiler still type-checks the registration because `Get<T>()` is a template.

---

## 3. The frame lifecycle

The engine thread drives the frame loop. The pattern is:

```cpp
AetherCore engine(config);

while (!engine.ShouldClose())
{
    engine.PumpEvents();
    engine.Tick(dt);                                 // game logic
    auto packet = engine.PrepareFrame(slot, frame);  // snapshot state
    engine.ExecuteRenderFrame(packet);               // submit to render thread
    // (game thread continues; render thread runs in parallel)
    frame++;
}

engine.WaitIdle();
```

The `RenderFramePacket` (`src/engine/rendering/RenderFramePacket.hpp:16`) is a value-typed snapshot of everything the render thread needs to draw the frame. It is:

- **Built on the engine thread** by `AetherCore::PrepareFrame` (`src/engine/AetherCore.cpp:282`).
- **Consumed on the render thread** by `AetherCore::ExecuteRenderFrame` (channel-based hand-off).
- **Deep-copied** - `std::vector<...>`, `glm::vec4`, etc. There are no data races with the next simulation tick.

Snapshot fields include camera matrices, sun/ambient/sky colors, point/spot light arrays, debug vertices, and stable GPU resource addresses.

### Render thread

`src/engine/rendering/RenderThread.hpp:25` owns a dedicated thread plus a bounded `coro::channel<RenderFramePacket>` of capacity 2 (double-buffered):

```
Game thread   : Sim N -> SubmitFrame(N) -> Sim N+1 -> SubmitFrame(N+1) -> ...
Render thread :              Read N -> Exec N            Read N+1 -> Exec N+1 -> ...
```

`SubmitFrame` returns in microseconds. The game thread continues with frame N+1's simulation while the render thread executes frame N. The channel is the synchronization point.

**Pause/resume:** `RenderThread::SetReloadInProgress(true)` makes the render thread finish the current frame and stop accepting new ones - used during hot-reload and similar critical sections.

---

## 4. The GPU abstraction

Engine code never includes `<vulkan/*>`. Two layers mediate:

| Layer | Directory | Headers |
|---|---|---|
| **Engine-facing API** | `src/engine/gpu/` | `GpuDevice.hpp`, `GpuTypes.hpp`, `BindlessManager.hpp`, `AsyncComputeContext.hpp` |
| **Vulkan implementation** | `src/engine/vulkan/` | `VulkanContext.hpp`, `GraphicsDevice.hpp`, `Swapchain.hpp`, `UniqueBuffer.hpp`, `UniqueImage.hpp`, `GpuHeap.hpp`, `GpuSpan.hpp`, `ResourcePool.hpp` |

The engine layer exposes the things engine code needs: format enums, frame lifecycle, command recording, bindless sampling. The Vulkan layer owns all `Vk*` handles, the VMA allocator, the swapchain, and the per-resource wrappers.

### `GpuDevice`

`src/engine/gpu/GpuDevice.hpp:23` is the entry point. Public surface:

- `Init(ServiceContainer&, Config)` - creates instance, device, swapchain, bindless manager, VMA.
- `BeginSwapchainFrame()` / `SubmitAndPresent(...)` - frame boundary.
- `RecreateSwapchain(Window&, bool vsync)` - handles window resize.
- `GetCurrentCommandRecorder()` - returns a `CommandRecorder` the engine can use to record GPU commands.
- `GetSwapchain*()` / `GetBindlessManager()` / `GetVulkanContext()` / `GetResourcePool()` - service accessors.
- `HasDedicatedComputeQueue()` - gates async compute enablement.
- `GetForwardColorFormat()` (static) - canonical forward color format (`R16G16B16A16Sfloat`).
- `SetSwapchainRecreatedCallback(std::function<void()>)` - hook for subsystem re-registration.

### Bindless resources

`BindlessManager` (`src/engine/gpu/BindlessManager.hpp`) allocates a single large descriptor set for sampled images and another for storage buffers/textures. Materials, textures, and vertex data are addressed by **bindless indices** in shaders, not bound per-draw. This is the central performance trick - almost no descriptor set work in the inner loop.

Each frame calls `AdvanceBindlessFrame(frameIndex)` to signal which descriptor slots are in use, enabling safety checks.

### `UniqueBuffer` / `UniqueImage`

Move-only RAII wrappers in `src/engine/vulkan/`. Both:

- Delete copy, define move.
- Have a static `Create(...)` factory returning `Expected<T>`.
- Auto-destroy on `GpuDevice::Shutdown` after `WaitIdle`.

This is the only way GPU memory is allocated outside of `GpuHeap`.

### `GpuHeap` / `GpuSpan<T>`

A device-local memory arena with a sorted free-list:

- `GpuHeap::Alloc<T>(n)` returns a `GpuSpan<T>` (typed view with BDA - buffer device address).
- Used by `MeshArena` and the animation database during asset loading.
- **Not thread-safe.** Only call from the asset loading thread.

### `ResourcePool`

`src/engine/vulkan/ResourcePool.hpp` is the render graph's virtual resource aliasing pool. The render graph can declare transient resources that get backed by a smaller pool of physical resources, reused across frames.

### Synchronization

- `volk` for Vulkan function loading (header-only mode, `VK_NO_PROTOTYPES` defined).
- `vk-bootstrap` for instance/device creation.
- VMA for GPU memory (`VMA_DYNAMIC_VULKAN_FUNCTIONS=1`).
- All barriers use `vkCmdPipelineBarrier2` (synchronization2).

### Destruction order

Always `m_gpu->WaitIdle()` before freeing GPU resources. VMA must outlive every allocation it manages - `GpuDevice::Shutdown` destroys VMA last. This invariant is enforced in `AetherCore::~AetherCore`.

---

## 5. The rendering pipeline

### Render graph

`src/engine/rendering/RenderGraph.hpp` holds a DAG of `RenderPass` nodes. Each pass declares:

- Inputs (color/depth attachments, buffers).
- Outputs (what it writes).
- Execution order (computed from the DAG).

This makes the rendering pipeline declarative: subsystems add passes, and the graph handles ordering and resource aliasing.

### Render passes

| Pass | File | Role |
|---|---|---|
| `CullPass` | `src/engine/passes/CullPass.hpp` | GPU frustum + occlusion culling. |
| `ForwardPass` | `src/engine/passes/ForwardPass.hpp` | Main forward PBR shading. |
| `SkyboxPass` | `src/engine/passes/SkyboxPass.hpp` | Sky/horizon/zenith gradient. |
| `PostProcessStack` | `src/engine/passes/PostProcessStack.hpp` | Tonemap, bloom, etc. |

`RenderingSubsystem` owns all of these and registers them in `RenderGraph`. Passes receive a `CommandRecorder` to record GPU work.

### Render queue

`src/engine/rendering/RenderQueue.hpp` collects `DrawCommand` structs on the engine thread (from the scene's ECS) and sorts them into a GPU-friendly order for the `ForwardPass`. The queue holds a reference to the `AnimationBlendSystem` / `AnimationRootMotionSystem` to inject skinning data per draw.

### `Renderer`

`src/engine/rendering/Renderer.hpp` is the per-frame state aggregator:

- Camera and lighting state.
- Point/spot light lists.
- World render data.

`Renderer` is updated by `CameraSubsystem` and `LightingManager`, snapshotted into `RenderFramePacket`, then consumed on the render thread.

### Shadows

Two shadow systems coexist:

- `ShadowService` (`src/engine/rendering/ShadowService.hpp`) - cascaded sun shadow maps, packed into an atlas (`ShadowAtlasManager`).
- `LocalShadowService` (`src/engine/rendering/LocalShadowService.hpp`) - point/spot light shadow maps.

`RenderQueue` cull/light visibility output drives both.

### Async compute

`src/engine/gpu/AsyncComputeContext.hpp` runs GPU skinning, particle update, and similar compute-heavy work on a dedicated compute queue when available. Gated by `HasDedicatedComputeQueue()` and `EngineSettings.graphics.asyncCompute`. The graphics/queue synchronization is via timeline semaphores (handles in `m_asyncSubmitSemaphore` / `m_asyncSubmitTimeline` on `AetherCore`).

---

## 6. The scene / ECS

- `src/engine/scene/World.hpp` wraps EnTT 3.16. Components live in `Components.hpp` (transform, mesh, material, light, physics, etc.).
- `src/engine/scene/Scene.hpp` is a higher-level façade that hosts renderable entities.
- `src/engine/scene/EcsHelpers.hpp` provides typed entity handles and convenience accessors.
- `src/engine/scene/SceneSubsystem.hpp` owns the `World` and `Scene` and exposes them through the service container.
- `src/engine/scene/System.hpp` defines the per-frame `Update(dt)` interface for game systems.

The render thread **never** touches the ECS - it operates on the `RenderFramePacket` snapshot. The engine thread writes to ECS, then the packet is filled and handed off.

---

## 7. The asset pipeline

### Virtual file system

`src/engine/io/FileSystem.hpp` mounts virtual paths:

- `assets://...` - packed PAK.
- `shaders://...` - shader sources (or their SPIR-V).
- (developer-defined) - raw directory mounts for hot iteration.

`PakBackend` reads from `.pak` files (zstd-compressed). `DirectoryBackend` reads directly from disk.

`IOThread` is a coroutine-driven async loader (`src/engine/io/IOThread.hpp`). It uses `aether::coro::async` and `aether::coro::channel` to overlap disk I/O with game-thread work.

### Asset processing

`src/engine/assets/AssetManager.hpp` and `AssetSubsystem.hpp`:

- `GltfAsset` loads glTF 2.0 via cgltf.
- `MeshArena` (in `AssetSubsystem`) uses `GpuHeap` for device-local vertex/index storage.
- `MaterialBuffer` (in `AssetSubsystem`) uploads PBR materials to a bindless storage buffer.
- `MeshUploadQueue` is the staging pipeline that pushes arena data into the GPU heap at the right frame boundary.

### Asset packer

`tools/assetpack/` is a CLI that:

1. Walks `resources/`.
2. Runs per-type processors (mesh optimization, texture compression, SPIR-V optimization).
3. Writes `build/data/assets.pak` with a hashed/xxHashed index.

The build system invokes it as a post-build step. PBR materials can be raw `.png` + `.toml` (`properties.toml`) or auto-imported from folder layouts.

---

## 8. The animation system

Three GPU-side systems share the same per-frame pattern:

- `AnimationBlendSystem` - clip blending, weight normalization.
- `AnimationRootMotionSystem` - extract root motion delta.

All three are initialized with `(allocator, device, capacity)`, register themselves on the service container, and are wired into `RenderQueue` so the forward pass can read the resulting skinning matrices via BDA.

`AnimationCompiler` (in `src/engine/animation/AnimationCompiler.hpp`) converts source animation data (`.aether`/Slang-friendly intermediate) into GPU-friendly runtime data using a worker pool from `AssetSubsystem::GetUploadPool`.

`AnimationDatabase` is the runtime storage for animation clips.

---

## 9. The UI system

`src/engine/ui/` is a self-contained immediate-mode UI system, similar in spirit to Dear ImGui but engine-integrated:

- `UISubsystem` is the orchestrator.
- `UiSystem` is the per-frame state machine (Begin / Window / End).
- `UiContext` is the user-facing draw API (`Button`, `Slider`, etc.).
- `UiWidgets` provides widgets.
- `UiLayout` is the flex/anchor layout engine.
- `UIRenderer` translates UI commands into render graph passes.
- `QuadRenderer` and `TextRenderer` are the underlying primitive renderers.
- `FontAtlas` and `TextRenderer` (in `src/engine/text/`) provide text rendering.

UI passes are re-registered on swapchain recreation through `GpuDevice`'s callback.

---

## 10. The physics layer

`src/engine/physics/` is a thin Jolt Physics wrapper:

- `PhysicsSystem.hpp` is the simulation step.
- `PhysicsComponents.hpp` defines ECS components (`RigidBody`, `Shape`, etc.).
- `PhysicsDebugRenderer` produces `DebugVertex` arrays that are routed to the render thread via `RenderFramePacket::debugVertices` and rendered by a `$PhysicsDebug` render pass.

---

## 11. Concurrency and coroutines

- **Engine thread** - calls `Tick`, `PrepareFrame`, `ExecuteRenderFrame`.
- **Render thread** - owned by `RenderThread`, drives `RenderGraph` execution.
- **IO thread** - coroutine-driven asset loading.
- **Animation compile pool** - thread pool from `AssetSubsystem`, used by `AnimationCompiler`.

Coroutines live under `aether::coro`:

- `Task<T>` / `async<T>` - coroutine return type.
- `Channel<T>` - bounded MPMC queue (`RenderThread` uses capacity-2).
- `Executor` / `inline_executor` / `queued_executor` - schedulers.
- `Sleep` - coroutine-friendly sleep.

Asset I/O and similar lazy work is written as coroutines and run on the IO thread's executor.

---

## 12. Error handling

AetherCore uses `std::expected<T, AetherError>` (C++26, polyfilled via `src/engine/utils/Expected.hpp`):

- `AE_TRY(var, expr)` - unwrap, return on error.
- `AE_EXPECT_OR_THROW(var, expr)` - unwrap, throw typed exception.
- `AE_UNEXPECTED(err)` - return failure.
- `AE_ASSERT(expr, msg)` - Debug-only check.
- `AE_ASSERT_ALWAYS(expr, msg)` - Release-grade assertion, logs and `std::abort()`s.
- `AetherError` carries a `LogCategory`, a message, and an optional error code.

All Vulkan calls in init paths return `Expected<T>`. Run-time render code uses `AE_ASSERT` and direct error checks.

---

## 13. Logging

`src/engine/utils/Logger.hpp` provides:

```cpp
AE_INFO(LogCategory::Engine, "Engine initialized with capacity {}", capacity);
AE_WARN(LogCategory::Asset, "Texture not found, using fallback");
AE_ERROR(LogCategory::Vulkan, "vkCreateImageView failed: {}", error.what());
```

Categories: `Engine`, `Vulkan`, `Asset`, `Render`, `Scene`, `Camera`, `UI`, `Input`, `Window`, `FileSystem`, `Animation`, `App`, `Validation`, `Std`, `Unknown`. New categories are added to `LogCategory.hpp`.

---

## 14. Key invariants

These are the rules the engine depends on. Breaking them will cause silent corruption or crashes.

1. **`m_gpu->WaitIdle()` before any GPU resource destruction.** Enforced in `AetherCore::~AetherCore`.
2. **Subsystem init order is significant.** See §1.
3. **The render thread never touches the ECS.** It operates on the `RenderFramePacket` snapshot only.
4. **`GpuHeap` is not thread-safe.** Only call from the asset loading thread.
5. **`ServiceContainer` is not thread-safe.** All `Register` / `Unregister` calls must be on the engine thread.
6. **VMA outlives everything it allocated.** `GpuDevice::Shutdown` destroys VMA last.
7. **`SetSwapchainRecreatedCallback` is the only place UI/render passes get re-registered** after a swapchain change.
8. **Never include `<vulkan/*>` in `src/engine/gpu/`, `rendering/`, or anywhere outside `src/engine/vulkan/`.** The abstraction is the contract.
9. **Never commit `.spv` files.** They're gitignored; the build regenerates them.
10. **`#pragma once` only.** No include guards.

---

## 15. Where to read next

- [`MODULES.md`](MODULES.md) - one-paragraph summaries of every subsystem.
- [`modules/`](modules/) - per-module deep dives.
- [`api/`](api/) - public type signatures.
- `AGENTS.md` (repo root) - coding conventions, naming, formatting.
