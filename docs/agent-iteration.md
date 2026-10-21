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

## Gameplay failure feedback

Native desktop and headless runs for C# and Rust return assertion evidence directly in the existing CLI/MCP AgentReport. Each failed assertion has an error diagnostic whose `message` is a JSON object and whose `source.file` identifies the copied snapshot in `artifacts`. A checkpoint failure links `checkpoint-N.json` even when execution stops before final `state.json`. Passing checkpoints also remain available as artifacts.

For example, `GAMEPLAY_ASSERTION_VALUE_MISMATCH` can contain:

```json
{"assertionIndex":0,"tick":90,"entity":"Result","type":"ReferenceResult","property":"score","expected":999,"actual":20,"actualPresent":true,"matchCount":1,"tolerance":0.0}
```

`tick` is completed simulation ticks; `assertionIndex` is zero-based within that snapshot's assertions. Parse the message with JSON to preserve escaped strings and exact integers. The stable failure codes are:

| Code | Meaning |
| --- | --- |
| `GAMEPLAY_ASSERTION_MISSING_ENTITY` | No observed entity has the requested name |
| `GAMEPLAY_ASSERTION_MISSING_TYPE` | The entity is present but the requested script/observation type is absent |
| `GAMEPLAY_ASSERTION_MISSING_PROPERTY` | Exactly one target matches but its property is absent |
| `GAMEPLAY_ASSERTION_AMBIGUOUS_TARGET` | Multiple entries match the entity and type |
| `GAMEPLAY_ASSERTION_VALUE_MISMATCH` | One property is available but differs from the expected value |

`actualPresent` distinguishes an absent property or ambiguous target from an actual JSON null. An ambiguous diagnostic does not choose one duplicate's value. The existing `$engine.exists` check continues to return false for a missing entity, so expecting true produces a value mismatch with actual false. Every failed assertion in the captured snapshot is reported; later checkpoints are not executed after a failure. Snapshot write failures report the I/O error without advertising an unavailable artifact. No new request flags or report schema version are required.

The MCP `gameplay_job_start` input schema now advertises the existing `launchConfig.profile` and `launchConfig.checkpoints` fields. Refresh/reconnect an already-running MCP client to load the updated tool schema. Runtime validation still rejects headless checkpoints, repeated/out-of-range checkpoint ticks and empty checkpoint assertions.

Windows child launches automatically recover missing `RUSTUP_HOME` and `CARGO_HOME` from the user's persisted Windows environment, with machine settings as a fallback. This fixes Rust jobs launched by desktop clients whose inherited environment predates the Rust setup. Explicit process overrides, including empty values, retain priority. PATH and the parent process remain unchanged. The same child-environment helper serves owned gameplay/compiler processes and the MCP owned-editor helper; Linux/macOS retain their inherited environment. No per-client environment edits are required. Rust must still have an installed, configured toolchain available through the normal executable lookup.

This removes the need to locate and open a sidecar merely to learn what failed. It does not measure a runtime speed improvement or change browser verification reports. Reproduce the real language/profile failure-and-repair checks after building `aether-project`:

```powershell
$env:AETHER_DESKTOP_REFERENCE_TESTS = '1'
python -m unittest discover -s tools/project -p test_desktop_reference.py -v
```

The opt-in suite uses copied reference projects under `.temp/agent-feedback/`, retains reports under `logs/agent-feedback/`, and owns its hidden runtime processes. It exercises final and checkpoint mismatches, missing entity/type/property, duplicate names, successful repair, headless evidence and detach failures for both languages.

## Reproduce iteration measurements

Build the engine CLI and bundled SDK first, then run:

```powershell
cmake --build --preset default --target aether-project
python tools/project/benchmark_iteration.py --repeats 3 --timeout-seconds 600 `
    --report logs/agent-iteration/measurement.json
```

The benchmark uses both checked-in reference projects, runs sequentially, creates named background sessions, closes owned runtime processes and retains every request, log and AgentReport. Failed or incomplete operations cannot produce passing medians. `--help` documents the bounded arguments. Avoid running another engine rebuild simultaneously: Windows cannot relink an active `aether-project.exe`.

Qualification evidence under `logs/agent-iteration/` includes semantic revisions 47/48/49, intentional compilation failures and repair for both languages, actual Cargo library/MSBuild assembly renames, cache-lock/concurrent-staging/cancellation tests, and byte-for-byte SDK staging checks. Native regression passed 1,463 cases and both GPU smokes with zero validation findings. All 41 browser cases and both Rust/C# target-aware Web publishes passed, including their relocated ZIP/iframe checks. The benchmark command itself passed a separate six-operation run. Cross-platform implementation is retained, but these new timing measurements were taken on Windows only.
