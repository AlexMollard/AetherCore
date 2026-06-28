# Module Overview

AetherCore is split into focused modules. Each module owns one concern; subsystems reach for siblings through the `ServiceContainer`.

> For a top-down view, see [ARCHITECTURE.md](ARCHITECTURE.md). For type signatures, see the per-module pages under [modules/](modules/) and [api/](api/).

---

## Engine core

| Module | Owns | Doc |
|---|---|---|
| **`engine/`** | `AetherCore` orchestrator, frame lifecycle, subsystem init/shutdown. | [modules/engine.md](modules/engine.md) |
| **`utils/`** | Logger, profiler, `Expected<T>`, `AE_TRY` macros, `EngineSettings`, frame pacer, `LoadingManager`, coroutines. | [modules/utils.md](modules/utils.md) |

## GPU + Vulkan

| Module | Owns | Doc |
|---|---|---|
| **`gpu/`** | Engine-facing GPU API. `GpuDevice`, `BindlessManager`, `AsyncComputeContext`, `GpuFormat`. **No `Vk*` types leak out.** | [modules/gpu.md](modules/gpu.md) |
| **`vulkan/`** | Vulkan implementation. `VulkanContext`, `Swapchain`, `UniqueBuffer`, `UniqueImage`, `GpuHeap`, `GpuSpan`, `ResourcePool`. The only module that includes `<vulkan/*>`. | [modules/vulkan.md](modules/vulkan.md) |

## Rendering

| Module | Owns | Doc |
|---|---|---|
| **`rendering/`** | `Renderer`, `RenderQueue`, `RenderGraph`, `RenderThread`, passes coordinator, frame constants, shadow services. | [modules/rendering.md](modules/rendering.md) |
| **`passes/`** | Individual render passes: `CullPass`, `ForwardPass`, `SkyboxPass`, `PostProcessStack`. | [modules/passes.md](modules/passes.md) |

## Game

| Module | Owns | Doc |
|---|---|---|
| **`scene/`** | ECS (EnTT), `World`, `Scene`, components, `SceneSubsystem`, per-frame systems. | [modules/scene.md](modules/scene.md) |
| **`assets/`** | `AssetManager`, `AssetSubsystem`, glTF loading, `MeshArena`, `MaterialBuffer`. | [modules/assets.md](modules/assets.md) |
| **`animation/`** | Skeletal animation, GPU skinning, `AnimationBlend` / `AnimationRootMotion`, `AnimationCompiler`, `AnimationDatabase`. | [modules/animation.md](modules/animation.md) |
| **`camera/`** | `Camera`, `CameraManager`, `CameraSubsystem`, `LightingManager`. | [modules/camera.md](modules/camera.md) |
| **`physics/`** | Jolt integration, `PhysicsSystem`, `PhysicsDebugRenderer`, physics components. | [modules/physics.md](modules/physics.md) |
| **`imgui/`** | Dear ImGui integration for debug/tooling UI. | [modules/ui.md](modules/ui.md) |

## Platform

| Module | Owns | Doc |
|---|---|---|
| **`platform/`** | `Window` (GLFW), `Input`, `CrashHandler`, `PlatformSubsystem`. | [modules/platform.md](modules/platform.md) |
| **`io/`** | Virtual file system, `PakBackend`, `DirectoryBackend`, `IOThread` (coroutine async I/O). | [modules/io.md](modules/io.md) |

## Rendering helpers

| Module | Owns | Doc |
|---|---|---|
| **`material/`** | PBR materials, `MaterialBuffer`, `BindlessContract`, `Texture`. | [modules/material.md](modules/material.md) |

## App

| Module | Owns | Doc |
|---|---|---|
| **`app/`** | `Application` main loop, `LayerStack`, layers (`DebugLayer`, `SandboxLayer`, `PhysicsLayer`, `FishingLayer`, `InventoryLayer`, `ScriptedSceneLayer`), game systems (day/night, fishing, physics, sandbox). | [modules/app.md](modules/app.md) |

## Tooling

| Module | Owns | Doc |
|---|---|---|
| **`tools/assetpack/`** | Asset packer CLI. Mesh/texture/SPIR-V processors. `import-materials` for auto-detecting PBR folder layouts. | - (see `tools/assetpack/README.md` if present) |

---

## Module dependency rules

- `gpu/` depends only on `utils/` and `vulkan/`.
- `vulkan/` depends only on `utils/`.
- `rendering/` depends on `gpu/`, `vulkan/`, `scene/`, `camera/`, `material/`, `utils/`, `io/`.
- `passes/` depends on `rendering/`, `gpu/`, `material/`.
- `scene/` depends on `utils/`, `gpu/`, `material/`, `io/`.
- `assets/` depends on `gpu/`, `vulkan/`, `io/`, `material/`, `utils/`.
- `app/` depends on everything (it is the composition root).
- **Cycles are forbidden.** The dependency graph above is enforced by `#include` discipline and review.

If you find yourself wanting to include something from a "lower" module into a "higher" one, the right answer is almost always to push the dependency down or to invert it (e.g. register a callback rather than call a function).
