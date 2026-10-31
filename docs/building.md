# Building and releases

[Back to the project overview](../README.md) · [First game tutorial](getting-started.md)

Build commands assume the repository root unless stated otherwise.

## Quick start

### Windows

Requirements:

- Visual Studio 2022 or newer with the Desktop development with C++ workload
- CMake 4.0+
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

A private Release package includes the editor, runtime, native dependencies, managed
SDK/compiler and a bundled .NET runtime. Native C# gameplay compilation uses bundled
Roslyn without a system .NET SDK. Rust, custom Slang authoring and Web export retain
their [per-action prerequisites](production-qualification.md). Clean-machine execution
must be qualified separately before promising that an artifact is self-contained.

No workflow in this repository runs automatically - not on push, not on a pull request,
and not on a tag. Hosted Windows minutes are billed, the build is long, and the same
build already happens locally. Verification is
[`scripts/Run-DebugGauntlet.ps1`](../scripts/Run-DebugGauntlet.ps1); GitHub Actions is a
manual, deliberate button.

Dispatch [`.github/workflows/release.yml`](../.github/workflows/release.yml) manually
from the Actions tab with the intended ref selected. It builds `Release`, stages the
payload, bundles a .NET runtime, verifies the result, and retains installer, portable
ZIP, symbols and provenance as private workflow artifacts. It does not create or
publish a GitHub Release, including when a tag is selected. Engine redistribution
terms remain deferred; see [distribution policy](DISTRIBUTION.md).

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
elevation. Projects and their generated build artifacts belong in the user's project
directory, outside the install directory.

**The .NET split.** A complete release package bundles the runtime and Roslyn compiler.
The editor compiles desktop C# gameplay in process for Play without an installed .NET
SDK. Source builds still need the .NET 10 SDK to build the engine's managed assemblies.
External IDE/MSBuild workflows also need that SDK; Visual Studio 2026 or a compatible
IDE is needed for the checked-in `net10.0` solution. C# browser exports have separate
SDK/workload requirements in [C# Web](csharp-web.md). Native Rust gameplay requires
the Rust toolchain, and browser Rust requires its WebAssembly target and the export
tools in [Web exports](web-export.md). These are separate actions: launching a complete
desktop package does not prove that an external toolchain or browser export is installed.

The current distribution is private. See [support and release policy](support-policy.md)
for target platforms, qualification gates and version/migration rules. A package should
only be described as SDK-free after its isolated delivered-product test passes.

**Model cache upgrades.** Model bake receipts now fingerprint source/dependency
contents and every derived output. A missing legacy receipt causes a rebuild.
Generated material names shared by different models must have identical contents;
conflicts stop the bake before replacing another model's material. For ambiguous
legacy ownership or intentional edits, use a uniquely named material or an authored
material override. Keep the previous project copy when resolving a conflict. Failed
writes retain ownership evidence so a corrected I/O failure can be retried.

**Packaged shader authoring.** Strict delivered-product mode requires a bundled
`slangc` and shader headers under `data/sdk/shaders` for custom Slang sources. A
package without these prerequisites reports the missing tools; it cannot silently
use the build machine's Vulkan SDK. Built-in compiled shaders do not need this tool.

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
