# Getting Started with AetherCore

This guide gets you from a clean clone to a running build. It supplements the build commands in `README.md` with the project layout, contribution workflow, and where to look when things go wrong.

---

## Prerequisites

### Required

| Tool | Version | Notes |
|---|---|---|
| **CMake** | 4.0+ | All builds go through CMake presets. |
| **C++ compiler** | MSVC 19.40+ (VS 2022 17.10) **or** Clang 18+ (clang-cl on Windows) | Clang uses C++26; MSVC uses C++23. |
| **Vulkan SDK** | 1.3+ | Set `VULKAN_SDK` if installed in a non-standard location. |
| **Git** | any | For fetching dependencies via CPM. |

### Platform support

| Platform | Compiler | Build generator | Status |
|---|---|---|---|
| Windows | MSVC | VS 2022 / 2026 | ✅ Primary target |
| Windows | Clang-cl | VS 2022 / 2026 | ✅ C++26 path |
| Linux | Clang 13+ | Ninja | ✅ Verified |
| macOS | - | - | Not supported (Vulkan only) |

### Optional

- **Tracy** - enabled by default (`AETHERCORE_ENABLE_TRACY=ON`); builds against the bundled profiler.
- **Slang** - enabled by default (`AETHERCORE_ENABLE_SLANG=ON`) for shader compilation.
- **AddressSanitizer** - opt in with `AETHERCORE_ENABLE_ASAN=ON` for sanitizer builds.

---

## Building

### Windows - Visual Studio 2022 + MSVC (default)

```powershell
cmake --preset vs2022-msvc
cmake --build --preset vs2022-msvc --config Debug
```

Output: `build/vs2022-msvc/src/app/Debug/App.exe` and `tools/assetpack/Debug/AssetPacker.exe`.

### Windows - Visual Studio 2022 + Clang-cl (C++26 path)

```powershell
cmake --preset vs2022-clang
cmake --build --preset vs2022-clang --config Debug
```

### Linux - Clang + Ninja

```bash
cmake --preset linux-clang
cmake --build --preset linux-clang
./build/src/app/App
```

Ubuntu/Debian dependencies:

```bash
sudo apt-get install build-essential cmake git ninja-build clang libvulkan-dev \
    vulkan-tools libglfw3-dev libglm-dev libfreetype6-dev
```

### Release builds

All presets default to `RelWithDebInfo`. To pick a different configuration:

```powershell
cmake --build --preset vs2022-msvc --config Release
```

### Useful CMake options

| Option | Default | Effect |
|---|---|---|
| `AETHERCORE_ENABLE_ASAN` | `OFF` | AddressSanitizer on all first-party targets. |
| `AETHERCORE_FAST_MSVC_DEBUG_INFO` | `ON` | `/Z7` + `/DEBUG:FASTLINK` for faster MSVC link. |
| `AETHERCORE_ENABLE_TRACY` | `ON` | Tracy profiler integration. |
| `AETHERCORE_ENABLE_SLANG` | `ON` | Slang shader compiler for `.slang` sources. |

Example:

```powershell
cmake --preset vs2022-msvc -DAETHERCORE_ENABLE_TRACY=OFF
```

---

## Running the demo

After a successful build:

```powershell
.\build\vs2022-msvc\src\app\Debug\App.exe
```

> **Note:** AetherCore requires a display. The `App` executable will not run in a headless environment. Use it on a workstation with a GPU and windowing system.

### Engine settings

Settings live in `engine.toml` (or whatever path you pass to `AetherCore::Config::settingsFile`). The default file is created on first run. Schema lives in `src/engine/utils/EngineSettings.hpp`. Typical fields:

```toml
[window]
width  = 1280
height = 720

[graphics]
vsync          = true
asyncCompute   = true
```

---

## Project layout

```
AetherCore/
├── src/engine/         Static lib "Engine" - all engine subsystems
│   ├── AetherCore.{hpp,cpp}   Engine orchestrator, frame lifecycle
│   ├── gpu/              GPU abstraction (GpuDevice, BindlessManager, AsyncComputeContext)
│   ├── vulkan/           Vulkan impl (VulkanContext, Swapchain, UniqueBuffer, UniqueImage, GpuHeap)
│   ├── rendering/        Render graph, passes, shadows, frame composer
│   ├── passes/           Individual render passes (Cull, Forward, Skybox, PostProcess)
│   ├── scene/            ECS (EnTT), World, Scene, SceneSubsystem
│   ├── assets/           Asset pipeline, glTF loading, mesh arena
│   ├── material/         PBR materials, bindless descriptors, texture loading
│   ├── camera/           Camera subsystem, camera manager, lighting manager
│   ├── physics/          Jolt physics integration
│   ├── animation/        Skeletal animation, GPU skinning
│   ├── ui/               In-engine immediate-mode UI (UISystem, UiContext, UIRenderer)
│   ├── platform/         Window (GLFW), input, crash handler
│   ├── io/               Virtual FS (PAK/directory backends), async coroutine I/O
│   ├── text/             Font atlas, text renderer
│   └── utils/            Logger, profiler, Expected<T>, AE_ASSERT, settings, frame pacer, loading manager
│       └── coro/         Coroutines: async<T>, task<T>, channel<T>, executor
│
├── src/app/             Executable "App"
│   ├── Application.{hpp,cpp}  Main loop, coroutine executor, loading manager, layer stack
│   ├── main.cpp               Entry point
│   ├── layers/                LayerStack, LoadingLayer, DebugLayer, etc.
│   └── systems/               Game systems (day/night, fishing, physics, sandbox)
│
├── tools/assetpack/     Asset packer CLI (mesh/texture/SPIR-V processing)
├── include/             Shared format headers (PakFormat, BinaryFormats)
├── shaders/             Slang shader sources (.slang)
├── resources/           Source assets packed at build time into assets.pak
└── CMake/               CMake modules (CPM, deps, target defaults, Slang integration)
```

### The two-layer GPU abstraction

Engine code never touches Vulkan directly. Two layers mediate:

| Layer | Directory | Key types |
|---|---|---|
| Engine-facing API | `src/engine/gpu/` | `GpuDevice`, `GpuFormat`, `BindlessManager`, `AsyncComputeContext` |
| Vulkan implementation | `src/engine/vulkan/` | `VulkanContext`, `GraphicsDevice`, `UniqueBuffer`, `UniqueImage`, `GpuHeap`, `Swapchain`, `ResourcePool` |

If you want to add a new GPU feature, add it to `gpu/` first, then implement it in `vulkan/`. The engine-facing API should never expose `Vk*` types.

---

## Development workflow

### Editing code

1. Edit `.cpp` / `.hpp` in `src/`.
2. Rebuild the affected target:
   ```powershell
   cmake --build --preset vs2022-msvc --config Debug --target App
   ```
3. Run the demo.

### Editing shaders

Shader sources live in `shaders/` (Slang `.slang` files). They are compiled by the Slang pipeline at build time and embedded into the asset pack. SPIR-V binaries are gitignored - never commit `.spv` files.

### Editing assets

Raw assets live in `resources/`. They are packed into `build/data/assets.pak` as a post-build step. To re-pack manually:

```powershell
.\build\vs2022-msvc\tools\assetpack\Debug\AssetPacker.exe pack resources build/data/assets.pak
```

### Updating the LSP database (clangd)

This project tracks `build-ninja-clang/compile_commands.json` for clangd. After adding/removing/renaming `.cpp`, `.hpp`, or `.h` files, or modifying any `CMakeLists.txt`, regenerate the database:

```powershell
cmake --preset clangd
```

---

## Common tasks

### "Where do I add a new subsystem?"

1. Create `src/engine/<area>/<Name>Subsystem.hpp/.cpp` mirroring the existing `*Subsystem` pattern (`PlatformSubsystem`, `SceneSubsystem`, `AssetSubsystem`, `RenderingSubsystem`, `UISubsystem`).
2. Add the subsystem to `AetherCore::AetherCore` in `src/engine/AetherCore.cpp` at the correct step in the init order (see "Subsystem init order" in `ARCHITECTURE.md`).
3. Register the subsystem (and any sub-services) on `m_services`.
4. Add a matching `Shutdown` call in the destructor (reverse order).
5. Update this docs directory and `MODULES.md`.

### "How do I add a new render pass?"

1. Create `src/engine/passes/MyPass.hpp/.cpp`.
2. Have the pass class own its pipeline + descriptor sets.
3. Register it in `RenderingSubsystem::RegisterPasses` (called from `Init`).
4. Add the pass to the `RenderGraph` so it's executed in the right order relative to other passes.
5. Document the new pass in `docs/modules/passes.md`.

### "How do I add a new engine log category?"

Log categories are declared in `src/engine/utils/LogCategory.hpp` as an `enum class LogCategory`. Add a new enumerator and the matching `LogCategoryTraits` specialization; `AE_INFO` / `AE_WARN` / `AE_ERROR` will pick it up automatically.

### "How do I run with a custom settings file?"

Pass it to `AetherCore::Config::settingsFile`:

```cpp
AetherCore::AetherCore({.appName = "MyGame", .settingsFile = "mygame.toml"});
```

---

## When something goes wrong

| Symptom | Where to look |
|---|---|
| Black / corrupted frame | `Swapchain::Recreate`, `GpuDevice::RecreateSwapchain`, `RenderGraph` topology |
| Crash on shutdown | `AetherCore::~AetherCore` - almost always a VMA / `GpuDevice` ordering issue. `m_gpu->WaitIdle()` must precede all subsystem destruction. |
| SPIR-V / shader compile error | Slang logs in the build output; look for the offending `.slang` file. |
| "Service not registered" panic | `ServiceContainer::Get<T>()` failed - the type isn't registered. Check `AetherCore::AetherCore` and the relevant subsystem's `Init`. |
| Validation layer errors | Enable Vulkan validation in your driver / SDK config. `AETHERCORE_ENABLE_ASAN` will catch CPU-side buffer overruns. |
| Asset won't load | Path is virtual (`assets://...`, `shaders://...`). Confirm the file is in the `assets.pak` and the virtual path is mounted in `io::FileSystem::InitializeDefaultMounts`. |
| Slow first frame | Asset packing, shader compilation, and PAK decompression happen on the first frame. Pre-warm via `LoadingManager` + `LoadingLayer`. |

For deeper debugging:

- **Tracy** captures a full timeline of engine + GPU work; run the Tracy profiler against the running build.
- **Vulkan validation layers** catch API misuse at runtime.
- **`AE_ASSERT`** logs and aborts in Debug; `AE_ASSERT_ALWAYS` is the Release-grade equivalent.

---

## See also

- [`ARCHITECTURE.md`](ARCHITECTURE.md) - engine design.
- [`MODULES.md`](MODULES.md) - subsystem overview.
- `AGENTS.md` (repo root) - coding conventions.
- `README.md` (repo root) - feature summary and quick build commands.
