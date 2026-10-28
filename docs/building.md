# Building and releases

[Back to the project overview](../README.md) · [First game tutorial](getting-started.md)

Build commands assume the repository root unless stated otherwise.

## Quick start

### Windows

Requirements:

- Visual Studio 2022 or newer with the Desktop development with C++ workload
- CMake 3.21+
- Vulkan SDK
- .NET 10 SDK for managed gameplay scripting

Configure and build the default MSVC development preset:

```powershell
cmake --preset default
cmake --build --preset default
```

This builds both the project hub and the editor. Start with the Launcher:

```powershell
cd build/default
./src/app/RelWithDebInfo/Launcher.exe
```

Return to the repository root before running the build commands below.

For ClangCL:

```powershell
cmake --preset vs2022-clang
cmake --build --preset vs2022-clang
```

Open `managed/AetherCore.slnx` in **Visual Studio 2026** or Rider to edit the engine managed code (the `AetherCore` SDK and `AetherCore.Interop` boot assembly). A project's gameplay scripts live in `projects/<name>/scripts/` and are edited there. The `net10.0` projects do not load in VS 2022, but VS 2022 can still build the native C++ targets through CMake.

### Linux

The Linux preset uses Clang, Ninja, and `RelWithDebInfo`:

```bash
cmake --preset linux-clang
cmake --build --preset linux-clang
```

Requires the Vulkan loader and driver, Clang, Ninja, and GLFW's platform development packages. The .NET 10 SDK is optional-managed scripting is skipped when it is absent.

### Useful presets

| Preset | Purpose |
|---|---|
| `default` | VS 2022 MSVC development build (`RelWithDebInfo`) |
| `vs2022-clang` | VS 2022 generator with ClangCL |
| `clangd` | Ninja + clang-cl compilation database and clang-tidy target |
| `linux-clang` | Linux Ninja + Clang development build |
| `vs2022-msvc-release` | Ship configuration |
| `vs2022-msvc-retail` | Retail configuration with LTCG and stripped diagnostics |

## Releases

A release is a self-contained editor: someone downloads one file, installs it, and builds
games without a compiler, the Vulkan SDK, or this repository.

No workflow in this repository runs automatically - not on push, not on a pull request,
and not on a tag. Hosted Windows minutes are billed, the build is long, and the same
build already happens locally. Verification is
[`scripts/Run-DebugGauntlet.ps1`](../scripts/Run-DebugGauntlet.ps1); GitHub Actions is a
manual, deliberate button.

To publish, tag the commit, push the tag, then dispatch
[`.github/workflows/release.yml`](../.github/workflows/release.yml) from the Actions tab
**with that tag selected as the ref**. It builds `Release`, stages the payload, bundles a
.NET runtime, verifies the result, and attaches an installer plus a portable zip to the
GitHub Release. Dispatched from a branch instead, it builds the same artifacts and
publishes nothing.

```bash
git tag v0.1.0 && git push origin v0.1.0   # publishes nothing on its own
```

What ships is declared in one place - the `_aether_payload` list in
[`src/app/CMakeLists.txt`](../src/app/CMakeLists.txt) - which drives both the build-tree
staging and the install rules, so the two cannot disagree. To reproduce a package locally:

```powershell
cmake --install build/vs2022-msvc --config Release --component Runtime --prefix staging
```

```powershell
./scripts/Verify-Package.ps1 -PayloadDir staging
```

The verifier is the guard against the failure mode that matters here: a package missing a
file that every developer's machine happens to have anyway, from a source tree or a
system-wide install, and which is therefore invisible until a stranger tries to use it.

The installer is per-user (`%LOCALAPPDATA%\Programs\AetherCore`), so it needs no
elevation and stays writable afterwards - MSBuild writes build artifacts beside the
bundled SDK when a project's scripts are first compiled.

**The .NET split.** The runtime is bundled, so the editor starts on a machine with no .NET
at all. The **SDK** is not - it is much larger, and it is only needed to *compile* a
project's C# scripts. The editor resolves `dotnet` at runtime and reports its absence
rather than failing a build opaquely, so an install without the SDK is a working editor
that cannot yet build scripts.

## Build targets

| Target | Output |
|---|---|
| `Engine` | Native static engine library |
| `Editor` | AetherCore editor executable |
| `Launcher` | Standalone project hub that starts a separate Editor for each selected project |
| `GameRuntime` | Standalone runtime executable (`AetherGame`) |
| `AssetPacker` | Asset import, conversion, and PAK command-line tool |
| `ManagedAssemblies` | Builds and deploys `AetherCore`, `AetherCore.Interop`, and project gameplay assemblies |

## Build tiers and diagnostics

| Tier | Configuration | Profiling and checks |
|---|---|---|
| Debug | `Debug` | Tracy enabled, full assertions |
| Dev | `RelWithDebInfo` | Optimized with Tracy enabled |
| Ship | `Release` | Profiling disabled, minimal checks |
| Retail | `Release` + `AETHERCORE_RETAIL=ON` | Diagnostics stripped, LTCG where configured |

Common options:

| Option | Default | Effect |
|---|---:|---|
| `AETHERCORE_ENABLE_ASAN` | `OFF` | AddressSanitizer on first-party targets |
| `AETHERCORE_ENABLE_TRACY_GPU` | `ON` | Vulkan GPU timeline instrumentation |
| `AETHERCORE_ENABLE_TRACY_PLOTS` | `ON` | Tracy plot and counter streams |
| `AETHERCORE_ENABLE_TRACY_MEMORY` | `ON` | CPU allocation and named GPU-memory reporting |
| `AETHERCORE_ENABLE_SLANG` | `ON` | Slang shader compilation when `slangc` is available |
| `AETHERCORE_RETAIL` | `OFF` | Retail stripping and optimization policy |

## Development notes

- Third-party dependencies are fetched with CPM and should not be edited under build directories.
- `clangd` users should regenerate `build/ninja-clang/compile_commands.json` with `cmake --preset clangd` after changing C++ source lists or CMake files.
- Verify locally before pushing - no workflow will do it for you: `./scripts/Run-DebugGauntlet.ps1` (GPU abstraction guard, build, unit suite, editor and runtime validation smokes). `ctest --test-dir build/default -C RelWithDebInfo -N` lists the suites registered for your configuration; these include `EngineTests` and, when their prerequisites are available, managed and gameplay API checks.
- Generated shader binaries and local tool state are intentionally excluded from source control.
