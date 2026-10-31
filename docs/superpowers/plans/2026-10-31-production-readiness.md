# Production readiness implementation plan

> **For agentic workers:** Use scoped implementation workers and independent reviews; preserve this ledger across context recovery. The user authorized commits and push after local qualification. No tag or release publication is authorized.

**Goal:** Close the Godot-comparison audit's implementation findings and provide repeatable, honest manual qualification for a private Windows/Linux desktop engine.

**Architecture:** Preserve the engine/GPU boundary and existing CMake-driven native/managed build. Make fallible mutations transactional, asset outputs verifiable, receive work bounded, and release qualification explicit and machine-readable. External hardware and clean-machine results are required evidence, never inferred from local tests.

**Tech stack:** C++23/26, Vulkan, CMake/CTest, .NET, Rust, PowerShell, Jolt, ENet.

**Spec:** [Readiness audit](../../audits/reports/AetherCore%20Godot%20readiness%20audit.md), `logs/godot-readiness-audit.json`.

## Global constraints

- Address the user as Pog Champ.
- No git state mutations or remote operations without a current-message request; implementation remains uncommitted.
- All workflows retain `workflow_dispatch` alone; no automatic triggers or hooks.
- Render thread never touches ECS; preserve lifecycle/GPU idle and VMA ownership requirements.
- Graph tools first for code discovery; read conventions before writing C++; prefix shell commands with `rtk`.
- Keep the engine private; do not select an open-source license or publish artifacts.
- First target is Windows and Linux desktop. Web remains an explicitly bounded existing capability, not a new release promise.
- Do not edit build/dependency outputs manually. Logs and scratch state stay ignored under `logs/` and `.temp/`.
- Existing untracked audit report must be preserved.

## Review focus

1. Failed animation replacement must retain old usable CPU/GPU state, not merely unpause.
2. Cache checks must detect deleted/changed dependencies and derived outputs, including body truncation and same-mtime edits.
3. Material collision handling must preserve authored overrides, intentional identical sharing and existing project references.
4. Failed restores, saturated physics and hostile network traffic must have bounded, observable behavior.
5. A missing suite, skipped phase or unavailable machine cannot produce a successful production-qualification claim.

## Task ledger

### 1. Transactional animation and exclusive mutation (R1)

**Files:** `src/engine/AetherCore.cpp`, `src/engine/animation/AnimationDatabase.*`, related animation/compiler/heap interfaces and tests.

- [x] Add deterministic failure tests for allocation/upload and exclusive-state cleanup.
- [x] Prepare replacement CPU/GPU data before publication; preserve previous state on recoverable failure.
- [x] Restore exclusive state on every recoverable exit; define controlled termination for irrecoverable state.
- [x] Validate actual script-facing path with fault injection where feasible.
- [x] Independent spec/quality review and covering tests.

### 2. Model bake identity and completeness (D1, D2)

**Files:** `tools/assetpack/{BakeAll,BakeOutputs,MeshProcessor,PakWriter}.*`, `src/editor/editor/ModelBake.*`, model/cache tests.

- [x] Add regression fixtures for missing companions, truncated outputs, same-mtime source/dependency changes and conflicting material names in both orders.
- [x] Share source/dependency/importer/output integrity validation across editor, batch and Web cache consumers.
- [x] Prevent destructive generated-material collisions, preserve intentional overrides/sharing, and propagate batch diagnostics.
- [x] Preserve native fresh-cook/collision-rejection protections and existing project identities.
- [x] Independent review and regression tests.

### 3. Recovery and strict compiler correctness (D3, PROD-01)

**Files:** `src/editor/editor/AutosaveRecovery.cpp`, `src/engine/utils/Expected.hpp`, recovery/error tests.

- [x] Reproduce failed replacement preserving recovery and destination; use shared atomic writer.
- [x] Keep recovery discoverable after a failed restore.
- [x] Handle Audio explicitly; verify strict Clang without demoting warnings.
- [x] Run focused tests and independent review.

### 4. Physics and network operating bounds (R2, R3, R4)

**Files:** `src/engine/physics/PhysicsSystem.*`, `src/engine/net/NetworkSubsystem.*`, consumer `src/app/net/NetworkSystems.cpp`, related settings/tests.

- [x] Surface Jolt saturation status with a bounded diagnostic/metrics policy.
- [x] Replace unexplained fixed scratch-abort behavior with explicit bounded configuration/fallback/admission policy and high-water evidence.
- [x] Bound receive events, bytes and processing time; keep packet ordering/control-event progress and avoid redundant payload copies.
- [x] Exercise limit/flood/slow-consumer behavior with meaningful tests.
- [x] Independent review; record measured supported envelope.

### 5. Complete manual verification and lifecycle gates (PROD-02, G-LIFECYCLE)

**Files:** `scripts/Run-DebugGauntlet.ps1`, release/CI workflow steps, new qualification helpers/tests as needed.

- [x] Require expected CTest inventory and prerequisites; run all registered suites, fail closed on omissions.
- [x] Add graceful close with deadline and validate teardown logs; force termination is a failed shutdown gate.
- [x] Add repeatable lifecycle/soak entry points with structured evidence and explicit skip/failure status.
- [x] Keep workflow triggers manual only.
- [x] Independent review and actual local runs.

### 6. Delivered-product and release identity gates (PROD-03, PROD-07)

**Files:** package verification/staging scripts, qualification scripts, release workflow steps.

- [x] Run staged/copied native products outside the source/build tree with documented prerequisites and isolated user state.
- [x] Exercise create/import/play/save/reopen/publish reference workflows where supported by control tooling.
- [x] Generate artifact checksums, dependency inventory/provenance, exact tool versions and retained symbol identities.
- [x] Support explicit signing verification without inventing credentials or publishing anything.
- [x] Distinguish local staging proof from clean standard-user VM/installer qualification.

### 7. Support, prerequisites, notices and maintenance (PROD-04/05/06/08, G-GPU/WEB/CONTROL/EDITOR)

**Files:** README/building/release/support documentation, private distribution/notice manifests, qualification status report.

- [x] Correct CMake and per-action .NET/Rust/Web prerequisites.
- [x] Record private distribution intent and Windows/Linux support targets separately from measured qualification.
- [x] Inventory third-party/license notices and stage applicable notices in engine/game packages.
- [x] Document version/backport/migration/private reporting policy, local-control trust and supported editor operations.
- [x] Supply hardware/browser/accessibility/device-failure checklists and executable checks where possible; do not turn untested cells green.

### 8. Final verification

- [x] All targeted regressions and all expected CTest suites.
- [x] Full Debug GPU gauntlet, including forced-off host copy and graceful shutdown.
- [x] Available Release/Retail, strict Clang and sanitizer builds/tests.
- [ ] Linux execution using an available local Linux environment; otherwise retain an explicit blocked qualification gate with runnable instructions.
- [x] Staged product/reference workflow and lifecycle/soak evidence.
- [x] Independent whole-change review, diff check, LSP synchronization, current readiness ledger and memory update.

## Execution record

- 2026-10-31: user approved the audit roadmap; chose private distribution and Windows/Linux desktop first. Starting at `da6824bd`; only the prior audit document is untracked.
- Work stayed in the current workspace because project instructions prohibit unrequested git mutations; the user subsequently authorized commits and push after qualification.
- Physical hardware/VM/signing/owner-policy gates remain required evidence; implementation of a harness does not satisfy them by itself.

- Local implementation and review evidence: see [qualification record](../../production-qualification.md).
- Full forced-off Debug gate passes all four suites (1,704 native cases and 171 managed tests); both smokes report zero GPU diagnostics and graceful exit. Explicit GPU fault/readback tests pass both paths.
- Release, Retail and strict Clang pass all four suites; ASAN passes all six, including actual heap-redzone and literal-registration probes. Dedicated native XML avoids interleaved logger output in qualification reports.
- Relocated product workflows, 19 editor mutation phases and 101 cycles per Full/UiShell/runtime profile pass with unchanged bundle inventories. These are local Windows results, not clean-machine or Linux qualification.
- The fresh-state runtime soak passes 3,601 seconds after readiness, graceful exit 0, log checks and unchanged bundle inventory. The user confirmed manually closing the earlier windows; those interrupted runs remain incomplete duration gates. Local private archive readback, checksums and source/native-symbol identity checks pass. Retained provenance describes the frozen qualification snapshot before subsequent documentation and commit metadata updates. Unchecked external gates are not waived.

## Continued local failure and workload qualification

The user requested continued work after `ef24dbb5` and `41b2fd95` were pushed.
This batch extends the existing recovery and physics acceptance gates. Changes were
initially kept uncommitted; the user subsequently requested commits and continued
work. It does not infer unavailable Linux/VM/hardware results.

- [x] Reproduce the candidate collision-suppression lifetime bug with two joints between one pair, including keep-both and remove-last controls; fix only after a failing native regression.
- [x] Reproduce and fix static motion replacement retaining joints against the destroyed body; check authored/script ownership and Dynamic/Kinematic behavior.
- [x] Exercise a bounded mixed ECS workload with joints, discriminating thin-wall CCD/control lanes, character grounding/jumping and real contacts. Record actual behavior and scratch/error metrics without claiming a capacity maximum.
- [x] Add real filesystem parent-creation and temporary-open failure regressions, preserving existing bytes, modification time and temporary-file inventory. Avoid test-only production APIs; write/flush/power-loss campaigns remain separate.
- [x] Review the changes independently, run affected native configurations and the full GPU gauntlet where engine code changes, synchronize LSP and update the qualification scope.

Only the root runs native builds, test executables and applications. Workers own
separate source areas; all full CTest suites run serially across configurations.

The continuation passes 1,711 native cases in all five local configurations, all
expected CTest suites (six with ASAN), the full forced-off Debug GPU gauntlet and
seven focused Debug cases / 2,853 assertions. See the qualification record and
`logs/production-continuation-final.json`. Earlier package/lifecycle/hour-long soak
evidence remains tied to the baseline bytes; no new distribution claim is inferred.
