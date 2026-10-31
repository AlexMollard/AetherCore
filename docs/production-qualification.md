# Production qualification record

AetherCore remains a private prerelease. Windows and Linux desktop are the first
release targets. Implemented guards and passing local checks do not establish
Godot-level maturity, a supported Internet service, or universal GPU compatibility.

## Local evidence

Evidence is retained under the ignored `logs/` directory on the qualification host.
Reports include failures as well as later successful runs; use their status and
source/artifact identities, rather than treating a report filename as a pass.

| Gate | Evidence / scope |
|---|---|
| Full Debug gate, host copy forced off | `production-qualified-debug-gauntlet.json`: all four CTest suites, 1,704 native cases; Editor and runtime ready, zero validation errors/warnings/advisories, graceful exit code 0 |
| GPU failure rollback and readback | `production-immutable-gpu-normal.log`, `production-immutable-gpu-forced-off.log`: five selected cases pass, 136 / 104 assertions; includes actual Vulkan animation failure injection and texture mip readback |
| Release, Retail and strict Clang | `production-qualified-release-final.json`, `production-immutable-retail-ctest.json`, `production-immutable-clang-ctest.json`: all four CTest suites and 1,704 native cases pass per configuration; managed suite includes 171 tests |
| ASAN effectiveness and complete suite | `production-qualified-asan-final.json`: all six suites and 1,704 native cases pass, including the heap-redzone and literal-registration probes; the runtime owns global allocations and ODR checking stays enabled |
| Physics scratch measurement | 128 bodies / 120 fixed steps with a forced 1 KiB arena: 9,028,416 total high-water bytes, 9,027,776 fallback high-water bytes; this is a measured workload, not a promise that every configured scene fits |
| Physics saturation branches | `production-pressure-{release,retail}.log`: isolated returned pair-cache bit 2 and solver-constraint bit 4. `production-manifold-{release,retail}.log`: opt-in pinned-Jolt sensor/compound fixture isolates bit 1 with a normal-capacity control and checks engine counter mapping; this is upstream-path evidence, not an ECS compound-shape feature |
| Physics scratch termination policy | `production-scratch-{release,retail}.json`: normal-budget child completes; 1 KiB arena / zero fallback child terminates with the exact exhaustion and always-on assertion diagnostics. Arbitrary crashes, timeouts and sanitizer diagnostics are rejected |
| Qualification harness regressions | Identity/schema/symbol, missing-suite, tamper, lifecycle, validation classification, sanitizer coverage and Web publisher fixtures have separate test output |
| Delivered package workflow | `production-product-immutable.json`: create/import/save, three play/stop/reload transitions, process reopen, publish and copied-game execution pass; source and copied payload inventories remain unchanged |
| Extended editor mutations | `production-editor-mutations-ready.json`: 19 phases pass, including create/reparent/TRS/component edits, synthetic ImGui Ctrl+D subtree duplication, delete, undo/redo, scene/process reopen and graceful close; no OS keyboard or accessibility claim |
| Shipping lifecycle | `production-immutable-full-uishell101.json` and `production-immutable-runtime101.json`: 101 cycles each for Full, UiShell and runtime, with graceful shutdown, isolated state, default delivered-marker behavior and unchanged bundle inventories; one second after readiness per cycle |
| Sustained operation | `production-immutable-runtime-soak-retry.json`: runtime profile, minimal seven-entity published scene, 3,601 seconds after readiness, graceful exit 0 without forced termination, clean log checks and unchanged bundle inventory. Working set at readiness / exit / peak: 467.43 / 478.63 / 478.66 MiB |
| Private archive identity | Portable and symbol ZIPs pass full archive readback and per-file checksums. `release-identity.json` binds source/tool/cache/dependency inputs, payload and matching native symbols; `qualification-payload-binding.json` proves all 295 delivered files other than refreshed provenance match the tested source package |

The exercised Vulkan device is AMD Radeon RX 9060 XT with AMD driver 26.9.1 on
Windows. Another integrated AMD device was detected but is not qualified by that
observation. No NVIDIA, Intel, old AMD, Linux GPU or clean-VM result is inferred.
Vulkan 1.4 and the backend's remaining mandatory features still apply. Host image
copy is optional; both paths are exercised on the available default device.

The successful runtime soak started at 07:28:54 UTC on 2026-10-31. Its 119
thirty-second telemetry samples are retained in
`production-immutable-runtime-soak-retry-samples.jsonl`; all recorded zero errors.
Sampled private bytes rose from about 1,392 MiB to 1,395 MiB. Memory growth is
reported rather than interpreted as proof of leak freedom. The earlier final-binary
run exited cleanly after about 31 minutes and remains an incomplete duration gate
(`production-immutable-runtime-soak.json`); the user confirmed manually closing both
game windows. The later
complete run does not qualify larger workloads, Full/UiShell hour-long operation,
or other hardware.

ASAN exposed a miniaudio 0.11.22 use-after-free on failed audio loading: resource
cleanup freed a node before notification cleanup read it. The build now generates
a header copy that runs notification cleanup before freeing the node, leaving the
pinned dependency download untouched. Regressions cover repeated missing/corrupt
clips and successful playback after repairing a failed clip. The rebuilt focused
audio run passes five cases / 150 assertions, and the final six-suite ASAN gate
passes with the corrected cleanup.

## Continued physics and recovery qualification

Further work after `41b2fd95` reproduced and fixed two joint-lifetime bugs:

- Removing one of several collision-suppressing joints between the same bodies
  enabled collisions too early. Authored and script joints now share a symmetric
  pair reference count; only the final suppressing joint releases the pair.
- Static-to-dynamic or static-to-kinematic replacement destroyed the old body while
  joints still referenced it. The transition now retires touching joints before
  destruction, matching collider rebuild behavior: authored declarations rebuild
  on the next flush, while touching script handles retire.

Both bugs have failing native regressions recorded before their fixes. Passing
controls cover reversed endpoints, mixed authored/script ownership, nonsuppressing
joints, repeated destruction, final release and actual body-index reuse.

`production-continuation-debug-gauntlet.json` passes the full forced-off GPU gate.
`production-continuation-{asan,release,retail,clang}-ctest.json` passes every expected
suite; each configuration runs 1,711 native cases. ASAN passes all six suites and
instrumentation covers 816 native compilation units. Seven focused cases pass
2,853 assertions in Debug. `production-continuation-final.json` binds the reports,
source inputs and separately retained native XML evidence.

The mixed ECS fixture exercises 74 bodies including eight character inner bodies,
16 joints and 240 fixed steps. It checks zero-gravity weld coupling, distance joints,
240 m/s projectiles blocked by thin walls against unobstructed controls, actual
floor/sensor contact identities, and character landing, walking, jumping and
grounding. With a 1 KiB arena and 16 MiB fallback budget, the first Debug run measured
9,030,736 bytes total scratch high water and 9,029,728 fallback bytes. Every sampled
step released live fallback bytes and recorded no saturation flags. This measures
the fixture's correctness and scratch use, not production capacity or latency.

New shared atomic-writer tests exercise successful complete/empty replacement,
parent creation blocked by a file, and a real Windows temporary-open failure at the
volume's filename-component limit. Failure preserves existing bytes, modification
time and directory inventory. No production fault-injection API was added.

The earlier lifecycle, hour-long soak and archive results describe their retained
baseline payloads. They do not establish long-running qualification of these later
physics changes. Linux, actual write/flush failure, power interruption and larger
workload campaigns remain separate gates.

## Repeating local checks

Run the full gate from the repository root:

```powershell
$env:AETHER_DISABLE_HOST_IMAGE_COPY = '1'
./scripts/Run-DebugGauntlet.ps1 -BuildDir build/vs2026-msvc -Config Debug `
    -Targets Editor,Launcher,GameRuntime,EngineTests,aether-ctl `
    -ReportPath logs/production-full-gauntlet.json
```

For an instrumented build, run `check_sanitizer_coverage.py` against its generated
compile database, then all CTest suites, including `ASANHeapRedzones`. The latter
must see the actual sanitizer diagnostic; an arbitrary child failure is insufficient.
Run native suites serially across configurations because loopback fixtures use fixed
ports. Run Release and Retail separately, including the assertion-disabled Jolt
saturation fixture. Assertions in the pinned Debug Jolt build stop before its error
return; a Debug classifier test does not qualify that runtime error path.

`Run-ProductQualification.ps1` checks relocated installed bytes in Unicode/spaced
paths, create/import/play/save/reopen/publish, and copied game execution. It requires
prepack `BUILD_PROVENANCE.json` and bundled .NET notices/runtime. Installed products
automatically disable developer fallbacks through their provenance/production
marker; this does not depend on a hidden test-only environment setting.

`Run-LifecycleQualification.ps1` supports Full, UiShell and runtime profiles,
per-process isolated state, exit deadlines and log checks. Retain 100+ lifecycle
cycles and a separate run longer than one hour, including exact scene, profile,
duration and memory measurements. A short smoke is not a soak, and a stable minimal
scene does not qualify larger streaming, joint, character or CCD workloads.

Generate staged provenance with `Write-ReleaseIdentity.py --stage-only` before
packaging. It binds source contents, compiler/cache/dependency inputs, payload
hashes and matching native symbol identities. Verify it with `--verify-stage`.
Generate final external identity after creating archives/installers; this avoids
circular artifact hashes. Keep nonignored source files unchanged between these
steps. Unsigned manifests provide traceability, not authenticity.

Retained local archives identify the source snapshot used during qualification.
Later documentation updates and commits do not rewrite that historical manifest;
release artifacts for a committed ref must be staged from that ref separately.

## Remaining release gates

| Area | Required qualification |
|---|---|
| Linux | Native build and all expected suites, real desktop GPU/lifecycle and installed-product execution; this Windows host has WSL but no installed distribution |
| Clean Windows/Linux machine | Standard-user installer and portable ZIP; no development tree, SDK/cache or undeclared global dependency; upgrade/uninstall preserve user projects and settings |
| GPU/driver breadth | Explicit OS/driver/device matrix, integrated/discrete AMD, NVIDIA and Intel; mandatory-feature rejection diagnostics, resize/minimize/DPI/multimonitor, repeated swapchain recreation |
| Failure handling | Device loss and allocation pressure; documented controlled termination where rollback is impossible; retained crash diagnostics and recovery evidence |
| Physics/load | Extend the bounded 74-body joint/CCD/character/contact fixture to larger realistic scenes, varied limits and sustained workloads; the manifold fixture is a pinned-Jolt sensor/compound case. Scratch fallback is bounded, and exhausting both limits remains fatal |
| Audio/input | Device unplug/default-device changes, mute and teardown; focus loss, key/button release, capture cancellation, keyboard navigation and accessibility |
| Editor/recovery | Create/import/delete/duplicate/reparent/transform/component edits, undo/redo, play/stop, autosave/recovery under write/replace failure, project migration and extension failure behavior |
| Public multiplayer | Authentication, abuse/load, fairness and operational ownership; bounded local transport tests do not qualify Internet hosting |
| Web | Existing profile/browser matrix and its documented C#/Rust restrictions; experimental status remains separate from desktop targets |
| Distribution | Owner-selected engine license remains deferred; inventory notices and exact runtime/tool inputs; signing credentials and signature checks if a signed release is promised |

The animation failure fixture uses real Vulkan operations and the production native
export/exclusive-mutation path in a lightweight runtime. It does not establish full
Editor/CoreCLR scheduling or visual animation continuity under injected failures.
Recovery tests exercise atomic replacement failure; shared atomic-writer tests also
cover parent creation and Windows temporary-open failure. Actual write/flush failure
and power-interruption campaigns remain separate. Model receipts and collision tests do
not establish concurrent-writer, Linux filesystem or actual browser presentation
behavior. Receive limits are cooperative between transport calls and do not promise
a hard wall-clock limit for an individual OS/ENet call.

Custom Slang authoring in an isolated package requires bundled `slangc` and shader
headers. Packages without them fail that action explicitly. Source-editor C# exports
also need a coherent bundled .NET runtime to produce a standalone game; publishing
from a properly staged editor supplies it. External Rust and browser toolchains
retain their separate documented prerequisites.

Editor C# compilation keeps its output in project/process-specific
`Builds/Intermediate/managed` directories instead of modifying the installed SDK.
These generated session directories currently require manual retention management;
there is no automatic inactive-session pruning or bounded disk-cache promise.
Safe automatic cleanup must account for live editors, detached compiler workers and
assembly-load-context lifetime before removing another session's files.

See [support policy](support-policy.md) for version/backport/migration rules and
[the implementation ledger](superpowers/plans/2026-10-31-production-readiness.md)
for the audit scope and ownership.
