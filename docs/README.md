# AetherCore Documentation

Welcome to AetherCore. This directory contains the project's reference documentation, written for contributors and engine users who want to understand how the engine fits together.

> AetherCore is a C++26/23 Vulkan game engine with a subsystem orchestrator, GPU abstraction layer, ECS-driven gameplay, Jolt physics, in-engine UI, coroutines, and a custom asset pipeline.

---

## Contents

### Start here

- **[Getting Started](GETTING_STARTED.md)** - prerequisites, building, running the demo, project layout, contribution workflow.

### Architecture

- **[Architecture Guide](ARCHITECTURE.md)** - the engine's high-level design, threading model, subsystem lifecycle, service locator pattern, GPU/Vulkan abstraction, render graph model, asset pipeline, and key invariants.
- **[Module Overview](MODULES.md)** - one-paragraph summaries of every engine subsystem with links to the detailed module docs.

### Module docs

- [`engine`](modules/engine.md) - `AetherCore` orchestrator, frame lifecycle, service locator
- [`gpu`](modules/gpu.md) - `GpuDevice`, `AsyncComputeContext`, `BindlessManager`, `GpuTypes`
- [`vulkan`](modules/vulkan.md) - Vulkan implementation, `VulkanContext`, `Swapchain`, `UniqueBuffer`/`UniqueImage`, `GpuHeap`, `GpuSpan`
- [`rendering`](modules/rendering.md) - `Renderer`, `RenderQueue`, `RenderGraph`, `RenderThread`, passes coordinator
- [`passes`](modules/passes.md) - `CullPass`, `ForwardPass`, `SkyboxPass`, `PostProcessStack`
- [`assets`](modules/assets.md) - `AssetManager`, `AssetSubsystem`, glTF loading, `MeshArena`, `MaterialBuffer`
- [`scene`](modules/scene.md) - `World`, `Scene`, ECS components, `SceneSubsystem`
- [`animation`](modules/animation.md) - skeletal animation, GPU skinning, blend/IK/root-motion systems
- [`camera`](modules/camera.md) - `Camera`, `CameraManager`, `CameraSubsystem`, `LightingManager`
- [`ui`](modules/ui.md) - `UISubsystem`, `UiSystem`, `UiContext`, widgets, layout, theming
- [`physics`](modules/physics.md) - Jolt integration, `PhysicsSystem`, `PhysicsDebugRenderer`
- [`platform`](modules/platform.md) - `Window`, `Input`, `CrashHandler`, `PlatformSubsystem`
- [`io`](modules/io.md) - virtual file system, PAK/directory backends, async coroutine I/O
- [`material`](modules/material.md) - PBR materials, bindless descriptors, textures
- [`text`](modules/text.md) - font atlas, text renderer
- [`utils`](modules/utils.md) - `Expected`, logger, profiler, `AE_TRY`, settings, frame pacer, loading manager, coroutines
- [`app`](modules/app.md) - `Application` main loop, layers, game systems

### API reference

- **[API Reference Overview](API_REFERENCE.md)** - index of public types and their responsibilities.
- Per-module API references live next to the module docs under [`api/`](api/).

---

## Reading order

If you're new to the codebase:

1. **[Getting Started](GETTING_STARTED.md)** - build it, run it, see the demo.
2. **[Architecture Guide](ARCHITECTURE.md)** - understand the design before reading code.
3. **[Module Overview](MODULES.md)** - pick the subsystem that interests you.
4. The specific module doc under [`modules/`](modules/).
5. The API reference under [`api/`](api/) when you need exact signatures.

If you're working on a specific subsystem, jump straight to that module doc.

---

## Conventions used in these docs

- `AetherCore.hpp:23` - file path and line number pointing to the canonical definition.
- `ClassName` - public type.
- `m_memberName` - instance member (matches the `m_snake_case` convention in code).
- Code blocks assume `namespace aether` unless otherwise noted.
- "Engine thread" = the thread that calls `AetherCore::Tick`. "Render thread" = the dedicated `RenderThread` worker.

See `AGENTS.md` at the repo root for the project's coding conventions.
