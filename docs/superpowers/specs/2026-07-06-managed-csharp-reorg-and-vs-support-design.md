# Managed C# Reorg + Full Visual Studio Support — Design

Date: 2026-07-06
Status: Approved (heavy reorg), implementing on branch `claude/fervent-goldberg-7ca574`.

## Goal

Two intertwined asks:

1. **Full VS support for game scripts** — live debugging (breakpoints in scripts while
   the engine runs), build-in-VS → hot-reload, edit-then-`F5`-in-game → rebuild+reload,
   NuGet packages in game scripts, and a clean hand-authored solution.
2. **AAA-clean managed layout** — the current setup mixes game code into `resources/`
   (the asset-pack folder), has no standalone C# solution, duplicates MSBuild settings,
   and bolts C# projects onto the CMake-generated C++ solution via
   `include_external_msproject` (source of the config-mapping bug fixed earlier).

## Decisions (locked with the user)

- **Restructure scope:** Heavy full reorg.
- **SDK boundary:** clean referenced assembly (`ProjectReference`, `Private=false`,
  compile-only), *not* a NuGet package — best for iteration speed, tooling, and keeping
  shared type-identity across ALCs simple. Structured so it *could* be packaged later.
- **Naming:** full re-namespace.
- **F5 rebuild pipeline:** "A+B unified" — VS build and F5 write to the same deploy dir
  and run the same incremental `dotnet build`; F5-rebuild is dev-only (falls back to
  reload-only when no SDK/source).

## Target layout

```
managed/
  AetherCore/               → AetherCore.dll          SDK (public gameplay API)          ns AetherCore
    AetherCore.csproj
    Animation.cs Camera.cs Effects.cs Entity.cs EntityScript.cs HideInInspector.cs
    Input.cs InputActions.cs Key.cs Log.cs Material.cs Physics.cs Renderer.cs Tags.cs World.cs
    Internal/Native.cs      (P/Invokes, was Interop/NativeMethods.cs; internal)
    Internal/Utf8.cs        (was Interop/Utf8.cs; internal)
    Internal/HostBridge.cs  (extracted host callback fn-pointers; internal)  ← breaks the cycle
    Properties/AssemblyInfo.cs  ([assembly: DisableRuntimeMarshalling], InternalsVisibleTo("AetherCore.Interop"))
  AetherCore.Interop/       → AetherCore.Interop.dll  ABI + host boot (hidden from game)  ns AetherCore.Interop
    AetherCore.Interop.csproj  (GenerateRuntimeConfigurationFiles + GC settings live here now)
    Bootstrap.cs            (the [UnmanagedCallersOnly] Init entry point the host binds)
    ScriptRegistry.cs
    Abi.cs                  (ManagedScriptApi, NativeHostCallbacks, PropertyValue, PropertyType)
    Properties/AssemblyInfo.cs  ([assembly: DisableRuntimeMarshalling])
game/
  AetherGame/               → AetherGame.dll          gameplay scripts                    ns AetherGame(.Systems)
    AetherGame.csproj       (ProjectReference AetherCore, Private=false, ExcludeAssets=runtime)
    Player.cs Spinner.cs ToySpawner.cs
    systems/CharacterController.cs FoxSystem.cs ThirdPersonCamera.cs
AetherCore.sln              (checked-in: the 3 projects above)
Directory.Build.props       (shared C# settings + ArtifactsPath; .csproj-guarded)
global.json                 (pins the .NET 10 SDK)
nuget.config                (pins nuget.org)
```

Game code leaves `resources/` entirely — the compiled `AetherGame.dll` is what the
engine loads; `.cs` source never belonged in the asset pak.

## Assembly split & dependency flow

Acyclic: **`AetherCore.Interop` → `AetherCore` ← `AetherGame`**. Game references only the
SDK. Key mechanics:

- **`Native` P/Invokes** move into the SDK as `internal`. `Bootstrap.Init` still registers
  the `AetherHost` `DllImportResolver` by calling `Native.RegisterResolver()` — reachable
  from Interop via `InternalsVisibleTo` — deterministically at boot, before any script
  P/Invoke.
- **Cycle break (`HostBridge`)**: `Log` is game-facing (must be in the SDK) but reads the
  native `Log` callback that `Bootstrap.Init` receives. So the *extracted* host callback
  function pointers (`Log`, `ReportScriptError`) live in an SDK-internal `HostBridge`;
  `Bootstrap.Init` copies them out of the incoming `NativeHostCallbacks*` into `HostBridge`
  (via `InternalsVisibleTo`). The ABI *structs* stay in Interop.
- **Type identity across ALCs**: the collectible `ScriptsLoadContext.Load` resolves
  `"AetherCore"` back to the host's already-loaded SDK assembly
  (`typeof(EntityScript).Assembly`) — same as today's `AetherCore.Managed` special-case,
  renamed.
- **NuGet in scripts**: `ScriptsLoadContext` gains an `AssemblyDependencyResolver` built
  from the game assembly path so package deps resolve and unload with the collectible ALC.
- `InternalsVisibleTo("AetherCore.Interop")` on the SDK lets the ABI layer touch
  `EntityScript.Bind`, `Utf8`, and `HostBridge`.

## Build / deploy / reload

- **Build unit**: CMake's `ManagedAssemblies` target runs `dotnet build` on the two leaf
  `.csproj` **directly** — `AetherCore.Interop.csproj` and `AetherGame.csproj` (each pulls
  in the SDK via `ProjectReference`). It deliberately does NOT build `AetherCore.sln`: the
  solution is an IDE-only convenience, so the build stays generator-agnostic and
  cross-platform and never depends on a solution file. (CMake cannot generate SDK-style
  `.csproj`, so those are hand-authored source on every platform — the one thing that must
  be checked in regardless.) `ArtifactsPath` routes output into the CMake tree; the
  Interop + Game output dirs are copied into `data/scripts/managed` (SDK dll, Interop dll +
  runtimeconfig, game dll + deps.json + NuGet deps), mirrored next to `App.exe`.
- **Deterministic ALC unload**: `ScriptRegistry` reload/teardown funnels through one
  `ResetRegistry()` that clears every static root (`s_types`/`s_props`/`s_defaults`/
  `s_typeNames`) and calls `Unload()` on the outgoing collectible context before the next
  one is built — so the game assembly + resolved NuGet deps unload promptly on every F5,
  rather than lingering until an incidental GC.
- **F5 rebuild hook (dev-only)**: `ScriptedSceneLayer::DoReload` (or the subsystem) runs
  the same incremental `dotnet build` before reloading, when a dev flag + SDK + source path
  are present (source csproj path baked via a CMake compile definition; empty in shipping →
  reload-only). Build failures surface through the existing error-toast path.

## IDE experience (cross-platform)

- **`AetherCore.sln`** (hand-authored, checked-in, classic format) is a cross-platform IDE
  convenience only — `dotnet`/MSBuild and Rider consume it on Windows/macOS/Linux, and the
  build never touches it. Open it in **VS or Rider** for full C# IntelliSense/refactor/debug
  across engine + game. **CLion** opens `CMakeLists.txt` for the C++ engine (pure CMake, no
  `.sln`); **VS Code + clangd** uses CMake's `compile_commands.json`. `.gitignore` keeps the
  generated C++ `.sln` ignored while allowing this one via `!/AetherCore.sln`.
- **Debugging**: `AetherGame` gets a `launchSettings.json` **Executable** profile pointing
  at the built `App.exe`, so `F5` in VS/Rider launches the engine with the managed debugger
  attached → breakpoints in `Player.cs` hit (portable PDBs already loaded via
  `LoadFromStream`). The path is forward-slashed and preset-specific
  (`../../build-vs2022-msvc/src/app/Debug/App.exe`); other presets/OSes repoint it. Mixed
  native+managed is a documented opt-in.
- **NuGet**: `PackageReference` in `AetherGame`; deploy + ALC resolver handle runtime load.

## Native + CMake changes

- `DotNetHost` boots from `AetherCore.Interop.dll` + `AetherCore.Interop.runtimeconfig.json`
  and binds `"AetherCore.Interop.Bootstrap, AetherCore.Interop"` / `Init` (was
  `AetherCore.Managed...`). Log/warning strings updated.
- `CSharpScriptingSubsystem` locates the deploy dir via `AetherCore.Interop.dll` and loads
  `AetherGame.dll` (was `AetherCore.Managed.dll` / `AetherScripts.dll`).
- **Remove `include_external_msproject` + the `AETHER_ADD_CSHARP_PROJECTS` option** — the
  checked-in solution is now the C# editing surface, so C# projects no longer get bolted
  onto the generated C++ solution. This supersedes the earlier PLATFORM/MAP_IMPORTED_CONFIG
  fix (that whole class of bug disappears because the C# projects leave the C++ solution).
- Comment references to the "AetherScripts assembly" in engine headers updated to
  "AetherGame".

## Shared props / SDK pin

`Directory.Build.props` centralizes `TargetFramework net10.0`, `Nullable`, `LangVersion`,
`AllowUnsafeBlocks`, `ImplicitUsings=disable`, `Deterministic`, `DebugType=portable`, and
`ArtifactsPath` (guarded to `.csproj`). Per-project keeps only unique bits (AssemblyName,
RootNamespace, GC/runtimeconfig on Interop, ProjectReference on game). `global.json` pins
the SDK; `nuget.config` pins the feed. `.gitignore` covers `artifacts/`, `bin/`, `obj/`.

## Migration map (rename/move)

| From | To |
|---|---|
| `managed/AetherCore.Managed/*` (public API) | `managed/AetherCore/*`, ns `AetherCore.Managed` → `AetherCore` |
| `managed/AetherCore.Managed/Interop/NativeMethods.cs` | `managed/AetherCore/Internal/Native.cs`, ns `AetherCore` (internal) |
| `managed/AetherCore.Managed/Interop/Utf8.cs` | `managed/AetherCore/Internal/Utf8.cs`, ns `AetherCore` |
| `managed/AetherCore.Managed/Interop/{Bootstrap,ScriptRegistry,Abi}.cs` | `managed/AetherCore.Interop/*`, ns `AetherCore.Interop` |
| `managed/AetherCore.Managed/Interop/AssemblyAttributes.cs` | split into both projects' `Properties/AssemblyInfo.cs` |
| `resources/scripts/*` | `game/AetherGame/*`, ns `AetherScripts(.Systems)` → `AetherGame(.Systems)` |

## Verification

- `dotnet build AetherCore.sln -c Debug` and `-c Release` succeed; assemblies resolve.
- Native reads: `data/scripts/managed` contains `AetherCore.dll`, `AetherCore.Interop.dll`
  (+runtimeconfig), `AetherGame.dll` (+deps.json).
- CMake configure of a VS solution no longer lists any C# project (no
  `include_external_msproject`).
- Adversarial multi-dimension review (ALC identity, native boot, re-namespace
  completeness, CMake deploy, hot-reload, NuGet resolver) before finishing.

## Out of scope (future)

- File-watcher auto-rebuild ("C" pipeline).
- Actually shipping the SDK as a NuGet package.
- Splitting `Native` into its own third assembly (unnecessary — internal to the SDK).
