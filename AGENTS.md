# AetherCore project instructions

## LSP / clangd

This project tracks a `build-ninja-clang/compile_commands.json` for use by clangd. Run the `/sync-lsp` command whenever a new `.cpp`, `.hpp`, or `.h` file is added, removed, or renamed, or when any `CMakeLists.txt` is modified. The command regenerates the compilation database so clangd stays accurate.

## Build & Commands

All builds go through CMake presets (see `CMakePresets.json`). The project compiles as C++26 on Clang, C++23 on MSVC.

```powershell
# LSP compilation database (clangd)
cmake --preset clangd

# Windows builds
cmake --preset vs2022-msvc && cmake --build --preset vs2022-msvc --config Debug
cmake --preset vs2022-clang && cmake --build --preset vs2022-clang --config Debug

# Release builds (all presets default to RelWithDebInfo)
cmake --preset default && cmake --build --preset default
```

| Preset | Generator | Compiler | Purpose |
|---|---|---|---|
| `clangd` | Ninja | clang-cl | LSP `compile_commands.json` only |
| `default` | VS 2022 | MSVC | Default dev build |
| `vs2022-msvc` | VS 2022 | MSVC | Windows production |
| `vs2022-clang` | VS 2022 | ClangCL | Windows (C++26) |
| `vs2026-msvc` | VS 2026 | MSVC | Future VS |
| `vs2026-clang` | VS 2026 | ClangCL | Future VS + C++26 |
| `linux-clang` | Ninja | clang++ | Linux |

### Config options

- `AETHERCORE_ENABLE_ASAN` — AddressSanitizer (default OFF)
- `AETHERCORE_FAST_MSVC_DEBUG_INFO` — `/Z7` + `/DEBUG:FASTLINK` (default ON)
- `AETHERCORE_ENABLE_TRACY` — Tracy profiler (default ON)
- `AETHERCORE_ENABLE_SLANG` — Slang shader compilation (default ON)

### Targets

- `Engine` — static library, everything under `src/engine/`
- `App` — executable, `src/app/`, the F5 startup project
- `AssetPacker` — CLI tool, `tools/assetpack/`

There are no registered tests (no `add_test()` calls, no `ctest` targets). The CI workflow references `ctest` but it's a no-op.

## Architecture Map

```
AetherCore/
├── src/engine/         Static lib "Engine" — all engine subsystems
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
│   ├── utils/            Logger, profiler, Expected<T>, AE_ASSERT, settings, frame pacer, loading manager
│   └── utils/coro/       Coroutines: async<T>, task<T>, channel<T>, executor
├── src/app/             Executable "App"
│   ├── Application.{hpp,cpp}  Main loop, coroutine executor, loading manager, layer stack
│   ├── main.cpp               Entry point
│   ├── layers/                LayerStack, LoadingLayer, DebugLayer, etc.
│   └── systems/               Game systems (day/night, fishing, physics, sandbox)
├── tools/assetpack/     Asset packer CLI (mesh/texture/SPIR-V processing)
├── include/             Shared format headers (PakFormat, BinaryFormats)
├── shaders/             Slang shader sources (.slang)
├── resources/           Source assets packed at build time into assets.pak
└── CMake/               CMake modules (CPM, deps, target defaults, Slang integration)
```

### Subsystem init order (from `AetherCore.cpp`)

1. Platform → 2. GpuDevice → 3. Scene → 4. Assets → 5. Cameras → 6. Rendering → 7. UI → 8. Async Compute → 9. Swapchain callback

**Shutdown** is the exact reverse, preceded by `m_gpu->WaitIdle()`.

### Service locator

`ServiceContainer` (`src/engine/utils/ServiceContainer.hpp`) wires subsystems together. Use `TryGet<T>()` to retrieve services. Subsystems register themselves in their `Init()` methods.

## Code Conventions

### Naming

| Element | Style | Examples |
|---|---|---|
| Namespaces | `snake_case` | `aether`, `aether::app`, `aether::coro` |
| Classes / Structs | `PascalCase` | `AetherCore`, `GpuDevice`, `RenderFramePacket` |
| Member functions | `PascalCase` | `Init()`, `ShouldClose()`, `BeginFrame()` |
| Member variables | `m_snake_case` | `m_services`, `m_gpu`, `m_frameIndex`, `m_allocator` |
| Static members | `s_snake_case` | `s_setObjectNameFn` |
| Function parameters | `camelCase` | `appName`, `frameIndex`, `drawSlot` |
| Local variables | `camelCase` | `asyncCompute`, `scaledDt` |
| Enum classes | `PascalCase` type & values | `LogCategory::Vulkan`, `GpuFormat::R16G16B16A16Sfloat` |
| Constants | `kPascalCase` | `kMaxFramesInFlight = 3`, `kInvalidOffset` |
| Template params | Single uppercase | `T`, `U` |
| Macros | `AE_UPPER_CASE` | `AE_ASSERT`, `AE_INFO`, `AE_TRY` |

### Headers

- Always `#pragma once` (never include guards)
- First include in every `.cpp`: the corresponding `.hpp` header
- Then STL (alphabetically), then third-party (angle brackets), then project headers (quotes, grouped by subsystem)
- `.clang-format` has `SortIncludes: Never` — manual ordering is preserved

### Error handling

- `Expected<T>` = `std::expected<T, AetherError>` (defined in `src/engine/utils/Expected.hpp`)
- `AetherError` carries a `LogCategory`, message string, and optional error code
- `AE_TRY(var, expr)` — unwrap Expected, return unexpected on failure
- `AE_EXPECT_OR_THROW(var, expr)` — unwrap or throw typed exception
- `AE_UNEXPECTED(err)` — short-hand for `return std::unexpected(err)`
- `AE_ASSERT(expr, msg)` — Debug-only check, logs + throws
- `AE_ASSERT_ALWAYS(expr, msg)` — Ships in Release, logs + `std::abort()`
- Vulkan calls return `Expected<T>`, caller uses `AE_TRY` in init paths

### Logging

```cpp
AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless capacity: {}", capacity);
AE_WARN(LogCategory::App, "Could not query refresh rate.");
AE_ERROR(LogCategory::Vulkan, "Vulkan error: {}", error.what());
```

Categories: `Engine`, `Vulkan`, `Asset`, `Render`, `Scene`, `Camera`, `UI`, `Input`, `Window`, `FileSystem`, `Animation`, `App`, `Validation`, `Std`, `Unknown`.

### Memory & ownership

- `std::unique_ptr` for exclusive ownership (subsystems, GPU resources)
- `std::shared_ptr` only for `ServiceContainer` internal ownership and coroutine shared state
- Raw pointers only for non-owning references (e.g. `ServiceContainer::TryGet<T>()` returns `T*`)
- All GPU resource wrappers (`UniqueBuffer`, `UniqueImage`) are **move-only** (copy deleted, move defined)
- Follow the existing `Foo(const Foo&) = delete; Foo(Foo&&) noexcept;` pattern

### Formatting

- Tabs for indentation, spaces for alignment (`.clang-format`: `UseTab: ForIndentation`)
- Brace wrapping on everything — after class, function, namespace, struct, enum, control flow, before else/catch
- Column limit 250
- `InsertBraces: true` — always wrap if/for/while bodies in braces
- Access modifiers at -4 indent relative to class body
- No doxygen — use `//` line comments only
- Class-level comments describe purpose and thread safety

## Vulkan Specifics

The engine uses a **two-layer abstraction**: engine code touches `gpu/` types only (never `Vk*`), while `vulkan/` contains the implementation.

| Layer | Dir | Key types |
|---|---|---|
| Engine | `src/engine/gpu/` | `GpuDevice`, `GpuFormat`, `BindlessManager`, `AsyncComputeContext` |
| Vulkan | `src/engine/vulkan/` | `VulkanContext`, `GraphicsDevice`, `UniqueBuffer`, `UniqueImage`, `GpuHeap`, `Swapchain`, `ResourcePool` |

### Resource management

- `UniqueBuffer` / `UniqueImage` — RAII, move-only, factory `Create(...)` returns `Expected<T>`
- `GpuHeap` — device-local arena with sorted free-list, `Alloc<T>(n)` → `GpuSpan<T>`, used during asset loading only (not thread-safe)
- `GpuSpan<T>` — typed view over GPU buffer with BDA (buffer device address)
- `ResourcePool` — render-graph virtual resource aliasing
- All Vulkan calls use `volk` (header-only mode, `VK_NO_PROTOTYPES` defined)
- VMA for all GPU memory (`VMA_DYNAMIC_VULKAN_FUNCTIONS=1`)
- `vk-bootstrap` for instance/device creation
- Synchronization uses `vkCmdPipelineBarrier2` (synchronization2)

### Destruction

Always `m_gpu->WaitIdle()` before freeing GPU resources. VMA must outlive all allocations it manages — `GpuDevice::Shutdown()` destroys VMA last.

## Dependencies

Managed via CPM (`CMake/CPM.cmake`). Key third-party libs:

| Library | Purpose |
|---|---|
| Vulkan SDK | Graphics API |
| GLFW 3.4 | Window / input |
| GLM | Math |
| vk-bootstrap | Instance/device setup |
| VMA 3.3.0 | GPU memory |
| volk | Vulkan loader (headers-only) |
| EnTT 3.16.0 | ECS |
| Jolt Physics 5.5.0 | Physics |
| Tracy 0.13.1 | Profiling |
| stb | Image loading |
| cgltf 1.15 | glTF loading |
| FreeType 2.14.3 | Font rasterization |
| zstd 1.5.6 | PAK compression |
| xxHash 0.8.2 | Asset hashing |
| toml++ 3.4.0 | Config parsing |
| bc7enc_rdo | BCn texture compression (packer only) |
| daScript 0.6.0 | Scripting |

Third-party sources live under `build/_deps/` (gitignored). Never modify them directly.

## Do Not

- Do not run the game binary directly; no display is available in agent context
- Do not modify `compile_commands.json` manually — use `/sync-lsp`
- Do not commit compiled shader SPIR-V binaries (`.spv` files are gitignored)
- Do not modify files in any `build*/` directory — they're fully regenerated by CMake
- Do not modify `CMake/CPM.cmake` or `CMake/get_cpm.cmake` — they're third-party
- Do not change `.clang-format` without explicit user request — it reformats the entire codebase
- Do not modify third-party sources in `build/_deps/`
- Do not change the subsystem init/shutdown order in `AetherCore.cpp` without understanding the dependency chain
- Do not add `.spv` files to the repo
- Do not use include guards — always `#pragma once`
- Do not use `SortIncludes` — include order is meaningful and manually maintained

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, invoke the `skill` tool with `skill: "graphify"` before doing anything else.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
