# ⚡ AetherCore

A C++26/23 Vulkan game engine with a subsystem orchestrator, GPU abstraction layer, ECS-driven gameplay, Jolt physics, ImGui debug tooling, coroutines, and a custom asset pipeline.

---

## ✨ Features

- **GPU abstraction layer** - `GpuDevice`/`GpuTypes` wrap Vulkan behind a generic interface; core engine code never touches Vulkan directly
- **Subsystem orchestrator** - engine decomposed into `RenderingSubsystem`, `SceneSubsystem`, `AssetSubsystem`, `CameraSubsystem`, `PlatformSubsystem`, and other focused services wired through a `ServiceContainer` service locator
- **Bindless resources** - materials, textures, and vertex data accessed via bindless descriptors and BDA (buffer device address)
- **GPU heap allocator** - device-local memory arena with typed `GpuSpan<T>` suballocations (GPU malloc/free), used by `MeshArena` and animation database
- **Render graph** - GPU culling, forward pass, tiled lighting, CSM shadows, post-processing, triple buffering
- **Async Compute** - dedicated async compute context for overlapping GPU work
- **ECS world** - EnTT-based entity/component system with typed entity handles
- **Jolt physics** - component-based rigid body and shape authoring
- **Animation system** - skeletal animation with GPU skinning pipeline
- **ImGui debug tooling** - dockable panels for render stats, scene inspection, viewport controls, day/night settings, and diagnostics
- **Offscreen rendering** - render-to-texture camera targets
- **Asset pipeline** - virtual file paths (`assets://`, `shaders://`), `.pak` bundles with zstd compression, PBR material presets via TOML
- **Asset processor** - mesh processing, texture compression, SPIR-V optimization in the asset packer
- **Tracy profiling** - integrated instrumentation via engine macros
- **Day/night cycle** - time-of-day driven lighting system
- **Coroutine system** - `async<T>`, `executor`, `queued_executor`, `channel<T>`, `sleep_for` under `aether::coro`; enables lazy async coroutines for asset I/O and render thread sync
- **Error handling** - `Expected<T>` wrapping C++26 `std::expected`, `AetherError` with typed log categories, `AE_ASSERT`/`AE_ASSERT_ALWAYS`, `AE_TRY`/`AE_EXPECT_OR_THROW` macros; all Vulkan calls, asset loads, and I/O operations are error-checked
- **Loading manager** - `LoadingManager` tracks async load progress for asset and coroutine workflows
- **Frame pacer** - `FramePacer` regulates game-thread cadence to a fixed target FPS with coarse-sleep + fine-spin timing

---

## 🗂️ Project Layout

```
src/engine/
  AetherCore.hpp/cpp       Engine orchestrator - owns all subsystems
  ServiceContainer.hpp     Service locator for subsystem wiring
  animation/               Skeletal animation system
  assets/                  Asset subsystem, glTF loader, asset manager
  camera/                  Camera objects, camera manager, camera subsystem
  gpu/                     GPU abstraction (GpuDevice, AsyncComputeContext, BindlessManager)
  io/                      Virtual file system, PAK/directory backends, coroutine-based async I/O
  material/                Material presets, texture loading, bindless descriptor management
  mesh/                    Mesh types, mesh arena, upload queue, primitive meshes
  passes/                  Render passes (cull, forward, post-process, skybox)
  physics/                 Jolt physics integration
  platform/                Window, input, crash handler, platform subsystem
  rendering/               Render graph, render queue, pipelines, shadow service,
                           lighting manager, frame composer, render thread (channel-based sync)
  scene/                   Scene graph, ECS helpers, world, scene subsystem
  imgui/                   Dear ImGui integration for debug/tooling UI
  utils/                   Logger, profiler, settings, text/ini parser,
                           debug GUI helpers, frame pacer, loading manager,
                           Expected<T>, AetherError, AE_ASSERT macros,
                           coroutine system (coro/)
  utils/coro/              Coroutine primitives: Task (async<T>), Channel,
                           Executor (inline/queued), Sleep
  vulkan/                  Vulkan context, swapchain, resource pools,
                           GPU heap, buffer/image wrappers, shader utils

src/app/
  Application.hpp/cpp      Main application loop with coroutine executor and loading manager
  main.cpp                 Entry point
  layers/                  AppLayer interface, LayerStack, SandboxLayer, DebugLayer,
                           FishingLayer, InventoryLayer, UiSandboxLayer,
                           PhysicsLayer
  systems/                 Game systems (day/night, fishing, physics, sandbox)

shaders/                   Slang shader sources
resources/                 Source assets (packed at build time)
  fonts/                   Font files
  materials/               PBR material TOML presets
  models/                  3D model source files
  textures/                Texture source images
tools/assetpack/           Asset packer with mesh/texture/SPIR-V processing
CMake/                     Dependency and helper modules
include/                   Shared format headers (PakFormat, AeBnFormat)
```

---

## 🔧 Building

### Windows

**Prerequisites:** Visual Studio 2022 or 2026, CMake 4.0+, Vulkan SDK

**Compiler support:** Clang-cl uses C++26. MSVC uses C++23 (MSVC does not yet support C++26).

```powershell
# VS 2022 + MSVC (default)
cmake --preset vs2022-msvc
cmake --build --preset vs2022-msvc --config Debug

# VS 2022 + Clang-cl
cmake --preset vs2022-clang
cmake --build --preset vs2022-clang --config Debug

# VS 2026 + MSVC
cmake --preset vs2026-msvc
cmake --build --preset vs2026-msvc --config Debug

# VS 2026 + Clang-cl
cmake --preset vs2026-clang
cmake --build --preset vs2026-clang --config Debug
```

### Linux

**Prerequisites:** Clang 13+, CMake 4.0+, Vulkan SDK, Ninja

Install dependencies (Ubuntu/Debian):

```bash
sudo apt-get install build-essential cmake git ninja-build clang libvulkan-dev vulkan-tools \
  libglfw3-dev libglm-dev libfreetype6-dev
```

Configure and build:

```bash
cmake --preset linux-clang
cmake --build --preset linux-clang
```

Run:

```bash
./build/src/app/App
```

> If you use a non-standard Vulkan SDK, set `VULKAN_SDK` in your environment before running CMake.

### CMake Options

| Option | Default | Description |
|---|---|---|
| `AETHERCORE_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer on all first-party targets |
| `AETHERCORE_FAST_MSVC_DEBUG_INFO` | `ON` | Use `/Z7` + `/DEBUG:FASTLINK` in Debug for faster MSVC iteration (VS 2022 and earlier) |

---

## 📦 Asset Pipeline

Assets under `resources/` are packed into `build/data/assets.pak` as a post-build step. At runtime, assets are accessed via virtual paths: `assets://...`, `shaders://...`, etc.

The asset packer (`tools/assetpack/`) now includes dedicated processors for:
- **Mesh processing** - optimizes vertex/index data for GPU upload
- **Texture processing** - compresses and mip-maps textures
- **SPIR-V processing** - optimizes compiled shader bytecode

### PBR Material Presets

Define reusable materials as TOML files under `resources/materials/`:

```toml
[material]
baseColorFactor = [1.0, 1.0, 1.0, 1.0]
metallicFactor  = 0.0
roughnessFactor = 0.9

[textures]
albedo            = "assets://textures/your_basecolor.png"
normal            = "assets://textures/your_normal.png"
metallicRoughness = "assets://textures/your_metalrough.png"
occlusion         = "assets://textures/your_ao.png"
emissive          = "assets://textures/your_emissive.png"
```

You can also use a folder layout (handy for downloaded PBR zips):

```
resources/materials/MyPBRFolder/
    properties.toml
    albedo.png
    normal.png
    roughness.png
```

### Auto-importing raw texture folders

```powershell
AssetPacker import-materials resources
```

or combined with packing:

```powershell
AssetPacker --import-materials resources build/data/assets.pak
```

The importer detects common naming patterns (`*_Color`, `*_NormalGL`, `*_Roughness`, etc.) and auto-generates `properties.toml` files, leaving any hand-authored ones untouched.

---

## 🔬 Profiling

Tracy is active in Debug and Dev (`RelWithDebInfo`) builds, and is automatically stripped in Ship/Retail (`Release`) builds via linker GC. The behavior is controlled by `src/engine/Defines.hpp`, which sets `TRACY_ENABLE` based on the build tier (`AE_CONFIG_DEBUG` / `AE_CONFIG_DEV`).

All the Tracy library symbols are always compiled into `TracyClient.lib`; the per-tier toggling happens purely through the preprocessor define, and the linker drops unreferenced code when Tracy is off.

Detailed sub-streams can be isolated independently:

```powershell
cmake --preset default -DAETHERCORE_ENABLE_TRACY_GPU=OFF
cmake --preset default -DAETHERCORE_ENABLE_TRACY_PLOTS=OFF
cmake --preset default -DAETHERCORE_ENABLE_TRACY_MEMORY=OFF
```

---

## 📚 Dependencies

Managed via CMake FetchContent / CPM:

| Library | Purpose |
|---|---|
| Vulkan SDK | Graphics API |
| vk-bootstrap | Vulkan instance/device setup |
| VulkanMemoryAllocator | GPU memory management |
| GLFW | Window and input |
| GLM | Math |
| EnTT | ECS |
| Jolt Physics | Rigid body simulation |
| stb | Image loading |
| cgltf | glTF model loading |
| FreeType | Font rasterization |
| Tracy | Performance profiling |
| libzstd | PAK compression |
| xxHash | Hash-based asset IDs |
| toml++ | TOML config parsing |
