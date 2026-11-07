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

## Continued input, audio and Debug-link qualification

Further native regressions reproduced three lifecycle defects: cursor movement on
focus return becoming a gameplay delta, stale voice handles matching voices in a
new audio subsystem session, and immediate music stop leaving an outgoing
crossfade audible. An additional focus regression delivers loss and gain between
two samples, as can happen within one event pump. The original three cases failed
15 of 80 assertions; the batched focus case failed two of 12. The corrected four
cases pass all 92 assertions, including subsequent real movement, audible PCM
controls, silence and decoder retirement.

Native input retains a resync latch until the first focused sample; the GLFW focus
callback also latches intervening focus loss. Browser sampling and cursor relock
policy retain their existing behavior. Voice generations now survive subsystem
and object replacement through a process-lifetime per-slot issuer. The existing
32-bit handle ABI still has a finite 15-bit generation: stale protection wraps
after 32,767 issuances of the same slot. Immediate music stop retires both tracks,
including an outgoing-only fade.

The first full gate caught an Editor shutdown fail-fast (`0xC0000409`) before the
physics destructor. Three debugger captures reproduced it: the valid vtable's
deleting-destructor jump thunk was absent from the executable's CFG target table.
A full relink of unchanged source exited normally through complete shutdown.
Debug Windows targets now use `/INCREMENTAL:NO` in both compiler branches while
retaining `/guard:cf` and `/CETCOMPAT`. This is a workaround for the observed
incremental-link artifact; it does not attribute memory corruption to physics or
disable the guard. Evidence includes `editor-failed-cfg-table.txt` and the
`editor-exit-triage-*` debugger records. Microsoft's
[/INCREMENTAL documentation](https://learn.microsoft.com/en-us/cpp/build/reference/incremental-link-incrementally)
describes the generated jump thunks and full-link option.

After the link-policy change, all five local configurations pass 1,715 native
cases and every expected CTest suite (six with ASAN, four otherwise), including
171 managed tests. ASAN instrumentation covers 816 native compilation units.
The full forced-off Debug GPU gate passes with zero validation findings and
graceful Editor/runtime exit code 0. `production-focus-events-retry-final.json`
binds seven source inputs, all five reports and retained native XML/JUnit evidence;
`production-focus-events-retry-debug-gauntlet.json` records the complete GPU gate.
The earlier failed report remains available for the shutdown investigation.

Focus tests replay the production sampling/event handlers; they do not establish actual OS focus
delivery, physical audio output, device unplug or default-device switching.
Earlier installed-product, lifecycle and hour-long runs remain tied to their
recorded baseline bytes.

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

## Initial Linux local qualification (2026-11-07)

This initial campaign was **incomplete**. Native builds and bounded functional
checks passed, but the delivered Launcher could not spawn the Editor on Linux.
The ordinary relocated-product gate failed at the Windows-only handoff. Direct
Editor execution was recorded separately. The subsequent handoff campaign below
closes that implementation gap; this section preserves the initial evidence.

The candidate uses source base `095f51af` plus 19 uncommitted code and harness file
changes. Qualification fixed Clang/libstdc++ delete declarations, portable
`va_list` initialization, missing integer/math declarations, platform executable
names, tile-map URI preservation and Unix static-nethost package verification.
Two runtime defects also reproduced:

- Timing and render-graph statistics both defined `aether::FrameStats`. GDB showed
  the timing function's 40-byte result invoking the other type's 120-byte
  constructor and overwriting its caller. Timing now uses `FrameTimingStats`, and
  a test includes both definitions to prevent recurrence.
- Linux executables exposed none of the C ABI functions needed by C# P/Invoke.
  Linux Editor and GameRuntime now enable executable exports. All 456 editor and
  407 runtime managed entry points are present; 49 editor-GUI declarations remain
  intentionally outside the runtime.

Publishing also deleted the editor's managed intermediate directory while an
asynchronous startup compile was writing there. Publish now uses the sibling
`managed-publish` directory. The direct Editor workflow verifies the current
editor's retained assembly survives publishing.

| Check | Retained result |
|---|---|
| Native Debug and Release | All four expected CTest suites pass; 1,708 executed native cases per configuration |
| Full Debug gauntlet | Editor/runtime readiness, graceful exit 0 and zero classified Vulkan errors/warnings/advisories |
| Managed ABI | Debug/Release Editor and runtime symbol inventories match their supported C# declarations |
| Direct Editor product workflow | 25 phases pass: import/entity persistence, three play/stop/reload transitions, save/process reopen, publish and copied-game execution; Launcher handoff excluded |
| Lifecycle | Three cycles each Full, UiShell and runtime; one second after readiness per cycle, graceful exit 0 and unchanged inventories |
| Additional opt-in probes | Window callback ownership passes 8 assertions; texture mip readback passes 77 assertions normally and 45 with host copy forced off |
| Windows regression | Full Debug gauntlet passes all four suites, 1,715 native cases and clean Editor/runtime GPU shutdown |
| Harness and archives | Linux 65 / Windows 67 harness checks; portable and three-file ELF-symbol tarballs pass complete per-file/hash/Unix-permission readback and build-ID binding |

The environment is an isolated Ubuntu 26.04 WSL2 distribution, an unprivileged
qualification user, Clang 21.1.8/C++26 with libstdc++ 15, Vulkan SDK 1.4.350.1,
Slang 2026.8 and bundled .NET 10.0.12. Rendering used llvmpipe CPU Vulkan on an
authenticated Xvfb/Openbox X11 desktop. Its driver lacks Tracy's required Linux
calibrated host clock, so GPU profiling is disabled with a diagnostic; rendering
and validation remain enabled. WSLg Wayland startup was observed separately, but
its automated close path was not qualified. No physical Linux GPU result follows.

The eight default-skipped cases are opt-in integration probes, not platform-only
omissions. Window callbacks and GPU texture uploads were run separately; real
network/router/STUN, asset-generation and Rust compilation probes remain unrun.
Imported-entity persistence is covered, but unresolved mesh-source/no-material
warnings remain in the fixture logs; no visual correctness claim is made. This
campaign does not establish 101-cycle/hour-long Linux stability or physical audio,
input and accessibility behavior.

Candidate archives live under
`C:/Users/alexm/AetherQualification/20261107-linux-candidate/archives`.
They require `GLIBC_2.43` and `GLIBCXX_3.4.34`; an older supported-distribution
baseline still needs its own build and qualification. The aggregate
`logs/linux-qualification-20261107/qualification-summary.json` hashes 34 evidence
files and keeps the full-product failure separate from passing direct-Editor
checks. Final platform reports use separate `linux-debug/` and `windows-debug/`
directories; preliminary parallel reports shared filenames and are not the final
evidence. Source provenance retains two Linux-generated shader dependency-order
rewrites as well as the intentional fixes. This documentation update follows the
artifact freeze and does not rewrite its historical source manifest.

## Linux Launcher handoff qualification (2026-11-07)

The ordinary delivered Launcher-to-Editor workflow now passes on Linux.
`LauncherProcess` uses `posix_spawn` with literal UTF-8 arguments, the executable's
working directory and a child-specific control-port environment. A private Unix
socket carries one-shot readiness after project/layer initialization and window
reveal. Launcher distinguishes pending startup, readiness and early exit; releasing
its tracking handle leaves a healthy Editor running. Exact-child waiters reap
terminated children. Windows retains its named-event handoff.

The qualification harness also handles Linux reparenting. A process-local
subreaper preserves ancestry under the isolated harness when Launcher exits
between polls. Exclusive port ownership, executable path and creation time remain
required. Native `waitpid` collects adopted children and their real exit status;
regressions verify rejection without ancestry and retain signal exit code 137 in
crash evidence. Previous subreaper state is restored even if cleanup throws.

| Check | Retained result |
|---|---|
| Linux Debug and Release | All four expected suites pass; 1,716 executed native cases in each configuration |
| Full Debug gauntlet | Guard, build, suites and both GPU smokes pass; zero classified Vulkan findings |
| Spawn/readiness regressions | Eight cases pass: literal arguments, child environment/cwd/FD isolation, pending/ready/exit, moves, handle release, failed exec cleanup and one-shot readiness |
| Ordinary relocated product | All 26 phases pass, including Launcher create/handoff, three play/stop/reopen transitions, process restart, publish and copied-game execution; source and copied inventories unchanged |
| Final candidate lifecycle | Three short cycles each Full, UiShell and runtime; graceful exit 0 and unchanged inventories |
| Additional opt-in probes | Window callbacks pass 8 assertions; texture readback passes 77 normally and 45 with host copy forced off |
| Windows regression | Full Debug gauntlet, all four suites and 1,715 native cases pass; clean Editor/runtime validation shutdown |
| Harness and delivery | Linux 76 / Windows 67 checks; portable and symbol archives pass complete file/hash/Unix-mode readback and ELF build-ID binding |

Authoritative evidence is
`logs/linux-launcher-qualification-20261107/qualification-summary.json` and its
hashed reports. The final product report is `product-frozen.json`; the Debug
reports are under separate `linux-debug-final/` and `windows-debug/` directories.
Earlier failed ownership attempts and superseded staging reports remain retained
and are excluded from the final evidence set.

Final private unsigned archives are under
`C:/Users/alexm/AetherQualification/20261107-linux-launcher-final-candidate/archives`.
The portable tarball is 59,422,814 bytes; matching native symbols are 225,695,505
bytes. `release-identity-frozen.json` binds their hashes to the source snapshot,
payload and ELF build IDs. Source base remains `095f51af` plus uncommitted fixes;
this documentation follows the artifact freeze. No commit, remote operation,
release or automatic CI trigger was made.

These are local WSL2, llvmpipe and Xvfb/Openbox results. The earlier environment
limits still apply: GLIBC 2.43 / GLIBCXX 3.4.34, no physical Linux GPU or native
Wayland lifecycle qualification, no clean installed-machine or long-duration Linux
claim, and unresolved mesh/no-material fixture warnings prevent visual acceptance.

## Remaining release gates

| Area | Required qualification |
|---|---|
| Linux | Qualify an explicit distribution baseline, physical desktop GPU/lifecycle, native Wayland operation and clean installed-product execution; extend bounded lifecycle checks to sustained workloads |
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
