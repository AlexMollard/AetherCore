# Rust gameplay for agents

Project gameplay language is selected at creation and recorded in `[scripting].language`. Missing metadata keeps the C# default. Rust desktop modules and C# assemblies both run through the shared gameplay host; native C# scripting, reload, serialization and publishing remain available.

```powershell
./scripts/Run-GameplayPipeline.ps1 -Operation create -Project .temp/MyJam `
    -Name MyJam -Language rust -Template blank_2d
./scripts/Run-GameplayPipeline.ps1 -Operation build -Project .temp/MyJam
./scripts/Run-GameplayPipeline.ps1 -Operation test -Project .temp/MyJam `
    -Scene Start -Ticks 90 -LaunchConfig ./my-test.json
./scripts/Run-GameplayPipeline.ps1 -Operation publish -Target web -Project .temp/MyJam
```

Find exact public declarations, registration macros and paired C#/Rust examples in
[Script APIs and examples](getting-started.md#script-apis-and-examples). The safe API
lives in `rust/aethercore/src/lib.rs`; `rust/aethercore-abi` is the raw host contract.

Author gameplay in the generated `scripts/src/lib.rs` using `aethercore`, `gameplay_script!`, `Script`, `Context`, and `export_gameplay!`. The SDK owns ABI string conversion and validates world/entity access. Callbacks return `Result`; propagate recoverable errors with `?`. Desktop uses the native module; Web statically links the selected project with abort panic behavior. A Rust panic terminates that browser session and produces a failure report, rather than unwinding into C++.

Use the existing scene/component TOML schema and project asset paths. An authored 2D scene declares `[scene] kind = "2d"`; a browser camera explicitly declares `projection = "orthographic"`. Public authoring services are also available through the editor MCP; scripts can generate the same files without opening an editor. The fresh distribution fixture uses the shared authored scene/asset generator and a separate module under `tools/project/autonomous_game.rs`, rather than copying an existing project's compiled game.

A test operation needs explicit state assertions in `launchConfig`; launching without assertions is not verification. Native runtime tests use hidden, named agent sessions with autosave off and bounded shutdown. Compiler errors include structured diagnostics and log artifacts. Repair the source and start another operation; Rust currently uses rebuild/relaunch rather than native hot reload. Use `-Async` and `gameplay_job_status`/`gameplay_job_cancel` for persistent agent operations.

Rust networking parity, advanced editor scripting and general native hot reload remain deferred. [C# browser support](csharp-web.md) is available through the same Web publisher with its documented SDK subset. Supported browser gameplay and additional-asset declarations are documented in [Web exports](web-export.md). Run [distribution qualification](distribution-qualification.md) for a reproducible create, deliberate error, repair, build, native test and browser export scenario.
