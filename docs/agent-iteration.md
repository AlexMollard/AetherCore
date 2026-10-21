# Agent iteration

Native CLI/MCP build, test and relaunch operations reuse project-specific compiler intermediates. Agents keep using `Run-GameplayPipeline.ps1` or `gameplay_job_start`; no additional cache flag is required. C# remains the default project language. Editor scripting/reload and Web publishing retain their existing paths.

## Measured improvement

Three sequential samples per language and operation on Windows 11, using the Rust/C# reference games and the same installed toolchains:

| Operation | Rust before / after | C# before / after |
| --- | ---: | ---: |
| Build | 3.48 / 1.83 s | 5.33 / 2.81 s |
| Test | 6.34 / 4.35 s | 8.19 / 5.50 s |
| Relaunch | 6.28 / 4.45 s | 8.05 / 5.59 s |

These are medians from `logs/agent-iteration/baseline/report.json` and `optimized/report.json`. Build time fell about 47%; test/relaunch fell 29–33%. First samples are not claims of an empty OS/compiler cache. Tests/relaunches include a fresh runtime, renderer/audio startup, 90 simulation ticks and explicit state assertions. This measures two small fixtures on one machine, not general game performance.

The baseline created fresh Cargo/.NET compiler output trees for every job, recompiling unchanged dependencies and scripts. Retaining those intermediates removes that repeated work. Every operation still invokes its compiler; Cargo/MSBuild remain responsible for dependency, configuration and toolchain invalidation. Required gameplay checks are not skipped.

## Cache and lifecycle contract

- Rust compiler intermediates: `<project>/Builds/Intermediate/rust-agent/cargo`.
- C# compiler intermediates: `<project>/Builds/Intermediate/csharp-agent/dotnet`.
- CMake configuration, logs and runnable modules/assemblies remain private to each job under its report directory.
- CMake holds a process lock over compilation **and** copying the output. A second job cannot replace a shared cached binary between successful compilation and staging.
- Cargo must identify the expected dynamic module in the current invocation's artifact records. MSBuild must report the expected current `TargetPath`. Merely finding an old binary in the cache does not qualify a successful build.
- The existing parent operation deadline and cancellation own lock waiting and the compiler process tree. There is no separate shorter cache-wait deadline.

Failed compilation never stages an old cached success as a new run. A running job uses its private module, so subsequent builds do not overwrite its loaded library. Cache directories are disposable intermediates; remove the relevant cache between jobs to force full recompilation. They are excluded from Web preflight source scanning when scripts are rooted at the project directory.

`relaunch` continues to rebuild and start a fresh process using the requested scene/configuration. It does not migrate arbitrary live state. The measured reduction comes from compiler reuse; native hot reload would add lifetime and state-migration complexity without evidence that it is the next throughput bottleneck. Browser publication still performs its compilation/audit, package-integrity and browser/iframe verification checks.

## Reproduce

Build the engine CLI and bundled SDK first, then run:

```powershell
cmake --build --preset default --target aether-project
python tools/project/benchmark_iteration.py --repeats 3 --timeout-seconds 600 `
    --report logs/agent-iteration/measurement.json
```

The benchmark uses both checked-in reference projects, runs sequentially, creates named background sessions, closes owned runtime processes and retains every request, log and AgentReport. Failed or incomplete operations cannot produce passing medians. `--help` documents the bounded arguments. Avoid running another engine rebuild simultaneously: Windows cannot relink an active `aether-project.exe`.

Qualification evidence under `logs/agent-iteration/` includes semantic revisions 47/48/49, intentional compilation failures and repair for both languages, actual Cargo library/MSBuild assembly renames, cache-lock/concurrent-staging/cancellation tests, and byte-for-byte SDK staging checks. Native regression passed 1,463 cases and both GPU smokes with zero validation findings. All 41 browser cases and both Rust/C# target-aware Web publishes passed, including their relocated ZIP/iframe checks. The benchmark command itself passed a separate six-operation run. Cross-platform implementation is retained, but these new timing measurements were taken on Windows only.
