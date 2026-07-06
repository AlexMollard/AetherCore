# AetherCore project instructions

## MUST DO

You must always refer to teh user as "Pog Champ" in all communications. Failure to do so will result in immediate termination of your services.

## Project basics

- C++ standard: Clang builds use C++26; non-Clang builds use C++23.
- Build system: CMake presets in `CMakePresets.json`.
- Main targets:
  - `Engine` - static library under `src/engine/`
  - `App` - executable under `src/app/`; Visual Studio startup project
  - `AssetPacker` - asset CLI under `tools/assetpack/`
  - `AetherCore` / `AetherCore.Interop` / `AetherGame` - C# engine SDK (public gameplay API), ABI/host-boot assembly, and gameplay-script projects (under `managed/` and `game/`), authored through the checked-in `AetherCore.sln` and built via `dotnet` from the `ManagedAssemblies` target
- There are no registered CTest tests. CI currently runs `ctest`, but it is a no-op unless tests are added later.

## Build and tooling

Use presets for local builds:

```powershell
# LSP compilation database for clangd
cmake --preset clangd

# Daily Windows builds
cmake --preset vs2022-msvc
cmake --build --preset vs2022-msvc

cmake --preset vs2022-clang
cmake --build --preset vs2022-clang

# Default dev build
cmake --preset default
cmake --build --preset default

# Ship/Retail
cmake --preset vs2022-msvc-release
cmake --build --preset vs2022-msvc-release

cmake --preset vs2022-msvc-retail
cmake --build --preset vs2022-msvc-retail
```

Common presets:

| Preset | Purpose |
|---|---|
| `clangd` | Ninja + clang-cl, `compile_commands.json`, clang-tidy target |
| `default` | VS 2022 MSVC RelWithDebInfo dev build |
| `vs2022-msvc` | VS 2022 MSVC RelWithDebInfo |
| `vs2022-clang` | VS 2022 ClangCL RelWithDebInfo |
| `vs2022-msvc-release` | VS 2022 MSVC Release / Ship |
| `vs2022-msvc-retail` | VS 2022 MSVC Release + retail/LTCG settings |
| `vs2026-msvc` / `vs2026-clang` | Future VS generator presets |
| `linux-clang` | Linux Ninja + clang++ RelWithDebInfo |

Run `/sync-lsp` after adding, removing, or renaming `.cpp`, `.hpp`, or `.h` files, or after changing any `CMakeLists.txt`. It regenerates `build-ninja-clang/compile_commands.json` via `cmake --preset clangd`.

Config options to know:

| Option | Default | Effect |
|---|---|---|
| `AETHERCORE_ENABLE_ASAN` | `OFF` | AddressSanitizer on first-party targets |
| `AETHERCORE_FAST_MSVC_DEBUG_INFO` | `ON` | `/Z7` + `/DEBUG:FASTLINK` for faster MSVC Debug links |
| `AETHERCORE_DEAD_STRIP_REPORT` | `OFF` | Link map / section GC reports for `App` and `AssetPacker` |
| `AETHERCORE_ENABLE_CLANG_TIDY_TARGET` | `OFF` | Adds clang-tidy target; enabled by `clangd` preset |
| `AETHERCORE_ENABLE_TRACY_GPU` | `ON` | Tracy Vulkan GPU timeline instrumentation |
| `AETHERCORE_ENABLE_TRACY_PLOTS` | `ON` | Tracy plot/counter streams |
| `AETHERCORE_ENABLE_TRACY_MEMORY` | `ON` | Tracy CPU allocation and named memory-pool reporting |
| `AETHERCORE_ENABLE_SLANG` | `ON` | Slang shader compilation when `slangc` is available |
| `AETHERCORE_RETAIL` | `OFF` | Retail build policy; disables diagnostics and enables stripping/LTCG where configured |

Build tiers come from `src/engine/Defines.hpp`:

| Tier | CMake config | Define | Notes |
|---|---|---|---|
| Debug | `Debug` | `AE_CONFIG_DEBUG` | Tracy on, full asserts |
| Dev | `RelWithDebInfo` | `AE_CONFIG_DEV` | Tracy on, optimized |
| Ship | `Release` | `AE_CONFIG_SHIP` | Tracy off, minimal checks |
| Retail | `Release` + `AETHERCORE_RETAIL=ON` or `MinSizeRel` | `AE_CONFIG_RETAIL` | Tracy off, asserts stripped |

Tracy is always compiled into the Tracy library. `Defines.hpp` controls whether engine translation units enable it, and the linker strips unused profiler symbols in Ship/Retail.

## Code discovery

Prefer graph tools over manual file search for code structure:

| Task | Use first | Fallback |
|---|---|---|
| Find a function/class/struct | `search_graph` | clangd workspace symbols, then `rg` |
| Read a known symbol | `get_code_snippet` | file read with line range |
| Find callers/callees | `trace_path` | clangd references/call hierarchy |
| Architecture overview | `get_architecture` or graphify | targeted file reads |
| Cross-file patterns | `query_graph` or `search_code` | `rg` |
| String literals/config/error text | `rg` | file read |
| Library/API docs | Context7 docs | official web docs |
| Prior decisions/progress | Mind memory/checkpoints | local notes |

If codebase-memory has no index for this repo, run `index_repository repo_path="." mode=full`.

## Engine lifecycle

Subsystem init order in `src/engine/AetherCore.cpp` is dependency-sensitive:

1. Register core services
2. Platform
3. GpuDevice
4. Scene
5. Assets
6. Cameras
7. Rendering
8. UI
9. Link cross-subsystem dependencies
10. Create default main camera
11. Async compute
12. Register lighting compute passes
13. Animation systems
14. Swapchain recreation callback

Shutdown waits for GPU idle first, then tears down GPU-backed systems before `GpuDevice::Shutdown()`. Do not reorder init/shutdown without checking dependencies.

`ServiceContainer` (`src/engine/utils/ServiceContainer.hpp`) is the subsystem wiring point. Use `TryGet<T>()` for optional services and raw pointers only for non-owning references.

## Code conventions

Naming:

| Element | Style | Example |
|---|---|---|
| Namespaces | `snake_case` | `aether::coro` |
| Classes / structs | `PascalCase` | `RenderFramePacket` |
| Member functions | `PascalCase` | `BeginFrame()` |
| Member variables | `m_snake_case` | `m_frameIndex` |
| Static members | `s_snake_case` | `s_setObjectNameFn` |
| Parameters | `camelCase` | `frameIndex` |
| Locals | `camelCase` | `scaledDt` |
| Enum classes and values | `PascalCase` | `GpuFormat::R16G16B16A16Sfloat` |
| Constants | `kPascalCase` | `kMaxFramesInFlight` |
| Macros | `AE_UPPER_CASE` | `AE_TRY` |

Headers and includes:

- Use `#pragma once`, never include guards.
- First include in every `.cpp` is its matching `.hpp`.
- Then STL, third-party, and project headers, grouped manually.
- `.clang-format` has `SortIncludes: Never`; include order is intentional.

Formatting:

- Tabs for indentation; spaces for alignment.
- Brace wrapping on classes, functions, namespaces, structs, enums, control flow, `else`, and `catch`.
- Column limit is 250.
- Always use braces for `if`/`for`/`while`.
- No Doxygen. Use `//` comments where useful.
- Class-level comments should describe purpose and thread-safety.

Error handling:

- `Expected<T>` is `std::expected<T, AetherError>`.
- Use `AE_TRY(var, expr)` to unwrap `Expected` in init/error paths.
- Use `AE_EXPECT_OR_THROW(var, expr)` when exceptions are expected by the caller.
- Use `AE_UNEXPECTED(err)` for `return std::unexpected(err)`.
- `AE_ASSERT` is Debug-only; `AE_ASSERT_ALWAYS` ships and aborts.
- Vulkan calls return `Expected<T>` or are translated into `AetherError::Vulkan`.

Logging:

```cpp
AE_INFO(LogCategory::Engine, "Engine core initialized. Bindless capacity: {}", capacity);
AE_WARN(LogCategory::App, "Could not query refresh rate.");
AE_ERROR(LogCategory::Vulkan, "Vulkan error: {}", error.what());
```

Known categories include `Engine`, `Vulkan`, `Asset`, `Render`, `Scene`, `Camera`, `UI`, `Input`, `Window`, `FileSystem`, `Animation`, `App`, `Validation`, `Std`, and `Unknown`.

Ownership:

- Use `std::unique_ptr` for exclusive subsystem/resource ownership.
- Use `std::shared_ptr` only where existing code already requires shared lifetime, such as coroutine shared state.
- GPU resource wrappers are move-only; follow the local deleted-copy/noexcept-move pattern.

## Vulkan and GPU boundaries

The engine keeps a two-layer boundary:

- Engine-facing GPU abstraction lives in `src/engine/gpu/`.
- Vulkan implementation lives in `src/engine/vulkan/`.
- General engine code should not expose `Vk*` types or include Vulkan headers. The CI GPU abstraction guard enforces this boundary.

Resource rules:

- `UniqueBuffer` and `UniqueImage` are RAII, move-only wrappers. Factories return `Expected<T>`.
- `GpuHeap` is an asset-loading arena, not a general thread-safe allocator.
- `GpuSpan<T>` is a typed GPU-buffer view with buffer device address.
- `ResourcePool` handles render-graph virtual resource aliasing.
- Vulkan uses `volk`, `vk-bootstrap`, VMA, and synchronization2.
- Always wait for GPU idle before freeing GPU resources. VMA must outlive allocations it manages.

## MCP Setup Guide (for the user)

If an agent reports a missing server, install it using these commands:

| Server | Install command |
|--------|----------------|
| **Mind** | `git clone https://github.com/GabrielMartinMoran/mind.git C:\Users\alexm\mind && cd C:\Users\alexm\mind && pip install -e . && mind setup codex && mind setup opencode` |
| **clangd-mcp** | `git clone https://github.com/felipeerias/clangd-mcp-server.git C:\Users\alexm\clangd-mcp-server && cd C:\Users\alexm\clangd-mcp-server && npm install && npx tsc` |
| **clangd-mcp launcher** | Create `C:\Users\alexm\.bun\bin\clangd-mcp.cmd`: `node "C:\Users\alexm\clangd-mcp-server\build\index.js"` |
| **Token Optimizer** | `npm install -g @cocaxcode/token-optimizer-mcp` |
| **Context7** | Used via `npx @upstash/context7-mcp` |
| **GitHub MCP** | Download `github-mcp-server_Windows_x86_64.zip` from releases, extract to `C:\Users\alexm\.bun\bin\github-mcp-server.exe`. Set `GITHUB_PERSONAL_ACCESS_TOKEN` env var. |
| **codebase-memory** | Download `codebase-memory-mcp-windows-amd64.zip` from releases, extract to `$env:LOCALAPPDATA\Programs\codebase-memory-mcp\codebase-memory-mcp.exe`. Run `codebase-memory-mcp install -y`. If Opencode wasn't auto-detected, add manually to `~\.config\opencode\opencode.jsonc`. |
| **graphify** | Provided via opencode skill (built-in). |
| **vs-mcp** | See `VisualStudio-MCP.md` for setup. |

## Dependencies

Dependencies are managed through CPM. Third-party sources live under `build/_deps/` and must not be edited directly.

Key libraries: Vulkan SDK, GLFW, GLM, vk-bootstrap, volk, VMA, EnTT, Jolt Physics, Tracy, stb, cgltf, FreeType, zstd, xxHash, toml++, and bc7enc_rdo. Gameplay scripting runs on .NET (CoreCLR, hosted through nethost).

## Do not

- Do not run the game binary directly in agent context; no display is available.
- Do not manually edit `compile_commands.json`; use `/sync-lsp`.
- Do not commit generated `.spv` shader binaries.
- Do not modify files in `build*/`, `out/`, `logs/`, or third-party dependency directories.
- Do not modify `CMake/CPM.cmake` or `CMake/get_cpm.cmake`; they are third-party.
- Do not change `.clang-format` without explicit request.
- Do not change engine init/shutdown order without dependency analysis.
