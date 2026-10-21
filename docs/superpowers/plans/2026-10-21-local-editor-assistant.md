# Local Editor Assistant Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver local in-editor authoring with Unsloth/Qwen, persistent rich chat, integrated Git, protected rollback, compile/test iteration and optional engine development; agent writes stay unavailable until source control and recovery are qualified.

**Architecture:** A native editor host owns scope, recovery and validated main-thread tool dispatch. A Python worker handles model streaming and conversation orchestration against the existing Unsloth app. Reuse the control catalogue, reflection, typed undo and gameplay jobs; Git supplies deliberate source history, while durable action checkpoints protect dirty and unsaved work.

**Tech Stack:** Existing C++23/26, CMake, Dear ImGui, nlohmann JSON, Python stdlib, installed Git, local Unsloth streaming API, and current Cargo/.NET gameplay backends.

**Spec:** [Approved scope](../specs/2026-10-21-local-editor-assistant-design.md). Read both documents. Scope approved on 2026-10-21; this request authorizes planning, not feature execution or Git changes.

## Global Constraints

- "Inference, history, project reads, tool execution and verification stay local."
- "Recovery must ship before agent mutation is enabled." Protection is a hard dependency, not later polish.
- "Do not add a vector database, another graph, a cloud fallback or a second collection of independently implemented editor actions."
- "Use Git for normal source-control history and the already-planned action receipts for local recovery checkpoints."
- "Automatic recovery capture uses local checkpoint records and performs no Git mutation."
- "No hooks or automatic CI are introduced." Workflows remain workflow_dispatch-only.
- "The model can inspect source control status/diffs and propose a restore, but cannot directly reset history, force-push, clean the worktree, delete checkpoints or invoke arbitrary Git commands."
- "If a document type cannot be captured and restored, block assistant mutation of that document."
- "Do not call raw handlers from inference threads." ECS/editor operations stay on the game thread; rendering consumes RenderFramePacket.
- "Never terminate the user's Unsloth app." Own the worker, not the inference app.
- Credentials remain session/environment-only; every endpoint/model/budget/Python setting is SettingsHome::User.
- No current-repository commits, pushes, branches or worktrees without a new explicit user request. Review diffs per task instead of automatic commits.
- CRLF, naming/include/ownership conventions, GPU boundaries and init/shutdown dependencies apply.
- Run `cmake --preset clangd` after new C++ files/CMake edits. Never edit generated compilation databases or dependency/build files directly.
- Arbitrary state-preserving script hot reload and new media/model generation are outside this feature.

## Review Focus

1. Dirty memory and disk differ: checkpoint both without Save; retain clean baselines and recreate GPU previews (Tasks 4-6).
2. Ancestor/descendant roots and aliases overlap across editor processes; exact-root hash locks are insufficient (Task 2).
3. Crash after target replacement but before receipt completion is uncertain; recover without the model and never replay blindly (Tasks 3, 5, 7).
4. Imports, settings, prefab saves and delayed jobs need complete effect footprints or stay unavailable (Tasks 8, 14-15).
5. Later human edits and Git-index changes must survive rollback; checkpoints are action boundaries, not whole-project snapshots (Tasks 1, 5-7).

## Delivery order and write gate

**A, Tasks 1-7:** Independently usable Source Control and recovery. No model writes exist.
**B, Tasks 8-12:** Shared executor, local model and read-only chat; mutation is refused.
**C, Tasks 13-16:** Qualified project authoring, build/test iteration, then live previews.
**D, Tasks 17-18:** Engine toggle, candidate builds, restart and fallback.
**E, Tasks 19-20:** Distribution/offline qualification and separate fine-tuning evaluation.

Every assistant mutation requires native checks for scope/generation, healthy source-control context, overlapping-root ownership, supported effects/documents, durable preimages and unchanged preconditions. Model arguments or copied qualification reports cannot satisfy this gate. A native, opaque, single-use ActionPermit authorizes one declared action and never travels to the worker as authority.

Require installed Git and an initialized local repository for assistant writes; setup is an explicit Source Control action. Dirty/staged work is allowed, but unresolved merge/rebase/conflict operations block writes. Missing prerequisites leave inspection/chat usable. Preserve existing external MCP behaviour; never expose raw mutating handlers to the model as a shortcut. Enable only tools with qualified effect/recovery adapters.

## File ownership and contracts

New files below are proposals; existing integration paths have been checked. Namespace `aether::editor::assistant`; Git namespace `aether::editor::source_control`. Signatures use `Json = nlohmann::json`, `Path = std::filesystem::path`, UTF-8 string IDs and Expected<T>. Define named DTOs in the owning header, with typed internal enums and stable boundary diagnostic codes.

| Area | Owner |
|---|---|
| Local process/Git | `src/editor/sourcecontrol/CommandProcess.*`, `GitService.*`, `SourceControlModel.*` |
| Ownership/recovery | `src/editor/assistant/RootOwnership.*`, `RecoveryTypes.hpp`, `RecoveryStorage.*`, `RecoveryService.*`, `RecoveryDocuments.*`, `RestoreController.*`, `RecoveryReadiness.*` |
| Native authority/tools | `src/editor/editor/ControlExecutor.*`, `src/editor/assistant/MutationCoordinator.*`, `AssistantFiles.*`, `AssistantSession.*` |
| Worker transport | `src/editor/assistant/WorkerProcess.*`, `WorkerProtocol.*`; `tools/assistant/protocol.py`, `model_client.py`, `worker.py` |
| History/context/iteration | `tools/assistant/history.py`, `context.py`, `session.py`, `gameplay_jobs.py` |
| Panels | `src/editor/debug/SourceControlPanel.*`, `AssistantPanel.*` |
| Engine development | `src/editor/assistant/EngineDevelopment.*`, `tools/assistant/editor_handoff.py` |
| Verification/training | `tools/assistant/probe.py`, `qualify_recovery.py`, `qualify.py`, `dataset.py`, `evaluate.py`, `train.py` |

`.*` means matching `.hpp/.cpp`. Keep core services independent of ImGui so EngineTests can link them without full application panels. Every task wires new production TUs into tests/CMakeLists where needed; test .cpp files auto-glob.

## Verification conventions

For native tasks: reconfigure as needed, run `cmake --preset clangd`, then `cmake --build --preset default --target EngineTests`. Confirm the configured executable path, then run the relevant prefix from the checkout root:

```powershell
./build/default/tests/RelWithDebInfo/EngineTests.exe --test-case="SourceControl*,RootOwnership*,Checkpoint*,RecoveryDocument*,RestorePlan*,ControlExecutor*,Assistant*,WorkerProcess*"
```

CTest registers EngineTests as one test; `ctest -R Assistant` does not filter doctest cases. Python uses `python -m unittest discover -s tools/assistant -p "test_*.py" -v`. Run focused RED before implementation, then the same assertions GREEN; a skipped required integration is not qualification. Use disposable repositories/projects and owned processes. Native assertion contracts below specify observable results, not mock-call counts.

### Task 1: Local Git backend and build ownership

**Files:** Create `sourcecontrol/CommandProcess.*`, `sourcecontrol/GitService.*` under src/editor; `tests/sourcecontrol/GitServiceTests.cpp`. Modify existing src/app/CMakeLists.txt and tests/CMakeLists.txt.

**Interfaces:** `RunCommand(executable, span<const string> argv, cwd, stop_token) -> Expected<CommandResult>` returns exit status/separate bounded stdout/stderr bytes. GitService: `Discover(Path) -> Expected<RepositoryInfo>`, `Status() -> Expected<RepositoryStatus>`, `Diff(DiffRequest) -> Expected<DiffResult>`, `Log(size_t limit) -> Expected<vector<CommitInfo>>`. RepositoryInfo identifies actual root and project-relative scope.

- [ ] Write real temporary-repository cases `SourceControl nested project uses actual root`, `SourceControl stage bytes preserved`, `SourceControl Unicode newline filename binary diff`, `SourceControl missing Git/repository`, `SourceControl argv metacharacters literal`. Assert `indexAfter == indexBefore`, exact filename bytes, binary metadata and bounded errors.
- [ ] Run SourceControl* RED. Implement direct argv capture using ProcessArguments.cpp ownership/quoting patterns; never shell-string RunProcessCapture for user paths. Parse NUL-delimited status with optional index refresh locks disabled; disable external diff/textconv execution and use literal argv/path delimiters for inspection. No remote contact.
- [ ] Exclude both assistant/ and sourcecontrol/ from APP_LAUNCHER_SOURCES: Launcher initially globs the entire editor tree. Editor auto-includes new files; tests require explicit core TUs. Keep GameRuntime unchanged.
- [ ] Run GREEN; build Editor, Launcher and GameRuntime to prove separation. No agent writer exists yet.

### Task 2: Canonical root authority and cross-process ownership

**Files:** Create assistant/RootOwnership.*, RecoveryTypes.hpp, tests/assistant/RootOwnershipTests.cpp and tools/assistant/test_root_ownership.py.

**Interfaces:** `CanonicalRoot(Path) -> Expected<Path>`; `RootOwnership::Acquire(span<const Path>, stop_token) -> Expected<RootLease>`. RootLease is move-only/OS-held. TurnIdentity contains sessionId, turnId, projectRoot, optional engineRoot, generation and mode. Mode is native session state.

- [ ] Write two-process cases for equal/parent-child/disjoint roots, case/junction/symlink aliases, reverse acquisition order, cancellation and killed holder. Assert `second.status == Contended` on overlap, disjoint succeeds, and a new holder succeeds after owner exit.
- [ ] Run RootOwnership* RED. Implement a briefly serialized overlap registry plus OS-held live leases; one hash lock per exact root is insufficient. Validate canonical parents for prospective files and recheck at mutation time. Git-root identity is not mutation-root identity. Define SessionRoots and MutationFootprint in RecoveryTypes.hpp now (files, document IDs, scene targets, generated outputs and runtime effects), so document recovery can consume them before Task 8.
- [ ] Add a test-only EngineTests helper mode for Python multi-process cases; no shipped endpoint. PID text alone must not decide ownership.
- [ ] Run native cases and `python -m unittest discover -s tools/assistant -p test_root_ownership.py -v` GREEN.

### Task 3: Durable checkpoint storage and action journal

**Files:** Create assistant/RecoveryStorage.*, RecoveryService.*, tests/assistant/RecoveryServiceTests.cpp and tools/assistant/test_recovery_crashes.py. Ignore `**/.aether/assistant/` in .gitignore.

**Interfaces:** Define CheckpointId, ActionId; FilePreimage `{path, existed, hash, blobId, permissions, kind}`; ActionIntent `{id, tool, roots, targets}`; ActionObservation `{status, postimages, diagnostics}`. RecoveryService: `BeginTurn(TurnIdentity, RootLease&) -> Expected<CheckpointId>`, `CaptureFile(CheckpointId, Path) -> Expected<FilePreimage>`, `PrepareAction(CheckpointId, ActionIntent) -> Expected<ActionPermit>`, `RecordOutcome(ActionPermit&, ActionObservation) -> Expected<void>`, `Reconcile() -> Expected<vector<InterruptedAction>>`.

- [ ] Write `Checkpoint capture failure zero changes`, `Checkpoint binary/missing preimages`, `Checkpoint late target captured before write`, `Checkpoint torn append recovers`, `Checkpoint pinned survives conversation deletion`. Assert exact bytes/existence, stable IDs and no replay of uncertain actions.
- [ ] Run Checkpoint* RED. Store schema-v1 manifests/journals and content-addressed binary preimages once under .aether/assistant/recovery/. Transcript references IDs; separate native recovery-journal ownership from worker transcript ownership. Validate record sizes/schema/hash/path on load.
- [ ] Implement temp-write, flush, atomic replace and platform directory/metadata durability; inject failures at each boundary. Return unavailable if required durability cannot be provided; rename alone is not durability proof.
- [ ] Kill helpers after durable preimage and after target replacement/before completion. Reopen without model; assert recovery available, uncertainty reported, no repeated actions. Run native cases and crash process tests GREEN.

### Task 4: Non-destructive dirty-document recovery

**Files:** Create assistant/RecoveryDocuments.*, tests/assistant/RecoveryDocumentTests.cpp. Modify existing DebugPanel.hpp, DebugLayer.*, MaterialGraphPanel.*, SpriteSlicerPanel.*, SpriteAnimationPanel.*, PixelArtPanel.*, PixelArtDocument.* under their current owners and the scene/tile dirty-state owner reached through DebugLayer.

**Interfaces:** IRecoveryDocument: `Describe() -> DocumentIdentity`, `Capture() -> Expected<DocumentSnapshot>`, `Restore(const DocumentSnapshot&, uint64_t expectedVersion) -> Expected<void>`. Snapshot includes kind, stable identity, version, authored payload, clean baseline and dirty state independently of disk. Registry: `FindAffected(const MutationFootprint&) -> Expected<vector<DocumentIdentity>>`, `CaptureAll(span<const DocumentIdentity>) -> Expected<vector<DocumentSnapshot>>` refuses unsupported dirty targets.

- [ ] Write `RecoveryDocument dirty scene disk unchanged`, `RecoveryDocument material baseline preserved`, `RecoveryDocument atlas animation pixel roundtrip`, `RecoveryDocument tile dirtiness`, `RecoveryDocument unsupported blocks`. Assert original disk bytes, authored memory and clean baseline restored; capture never calls Save.
- [ ] Run RecoveryDocument* RED; register adapters at document owners and connect their snapshots to RecoveryService using `CaptureDocuments(CheckpointId, span<const DocumentSnapshot>) -> Expected<void>`. Reuse CaptureScene/RestoreSceneInPlace for recovery separately from typed UndoStack. Remap selection/handles and invalidate stale history where necessary.
- [ ] Serialize no GPU handles/futures/pointers. Cancel/join pending preview compilation before material restoration; recreate transient previews. Preserve or explicitly invalidate pixel-local undo; do not fabricate old operations.
- [ ] Run GREEN and real owned-editor dirty-document round trips. An unsupported adapter remains blocked until its live recovery passes.

### Task 5: Conflict-aware restoration state machine

**Files:** Create assistant/RestoreController.*, tests/assistant/RestoreControllerTests.cpp; extend RecoveryService/document registry.

**Interfaces:** `PlanRestore(CheckpointId, RestoreScope::Turn|Since) -> Expected<RestorePlan>` records targets, after-hash/existence preconditions and conflicts. `ApplyRestore(const RestorePlan&, RootLease&) -> Expected<RestoreResult>`; `ResumeInterruptedRestore(RestoreId) -> Expected<RestoreResult>`. Result includes completed/conflicted/interrupted status and pre-restore checkpoint ID.

- [ ] Write `RestorePlan earlier dirty/staged preserved`, `RestorePlan later overlapping edit conflicts`, `RestorePlan create delete rename binary`, `RestorePlan since boundary preserves unrelated`, `RestorePlan interrupted reopens`. Assert Git index/HEAD/branch unchanged, unrelated new files retained, recovery steps reconcile idempotently.
- [ ] Run RestorePlan* RED. Reverse recorded assistant deltas, not whole-project snapshots. Capture durable recovery before restore; stop Play/jobs and revalidate each target. Never blanket reset/clean/stash.
- [ ] Refresh assets/scripts/selection/documents; keep failed/partial receipts. Recovery must itself be recoverable.
- [ ] Run GREEN, crash checks and typed undo regression. Ctrl+Z remains in-place typed commands, not a snapshot fallback.

### Task 6: Independent Source Control and Recovery panel

**Files:** Create debug/SourceControlPanel.*, sourcecontrol/SourceControlModel.*, tests/sourcecontrol/SourceControlModelTests.cpp. Modify GitService, DebugLayer registration/project callbacks and unsaved-work handling.

**Interfaces:** GitService adds user-only typed Initialize, StagePaths, UnstagePaths, CommitStaged, CreateBranch, SwitchBranch, ReadCommittedFile. SourceControlModel owns view state and explicit UI-action queue. No arbitrary Git command is model-facing; compose existing project-open callback.

- [ ] Write real cases `SourceControl commit exactly displayed stage`, `SourceControl switch respects dirty docs`, `SourceControl no silent nested init`, `SourceControl external index invalidates action`, `SourceControl missing Git recovery browse`. Assert existing staging choices survive refresh and commit scope is shown.
- [ ] Run RED; implement status, text/binary diffs, history, staging/commits/branches, checkpoint pinning/storage, comparisons and Task 5 restoration. Repository policies apply; no hooks, automatic remote operations or model destructive Git tools.
- [ ] Keep panel usable without model/worker. Explicitly offer setup when repo absent. User commits use Git; automatic checkpoint capture performs no Git mutation.
- [ ] Run SourceControl* GREEN and real owned-editor UI actions against a disposable repo. Inspect resulting files/index/dirty memory, not just buttons.

### Task 7: Protection qualification and readiness gate

**Files:** Create assistant/RecoveryReadiness.*, tests/assistant/RecoveryReadinessTests.cpp, tools/assistant/qualify_recovery.py and test_qualify_recovery.py. Extend test wiring/local verification only.

**Interfaces:** `RecoveryReadiness::Inspect(const SessionRoots&, const RequiredEffects&) -> ReadinessReport` reports Git/context/storage/adapter readiness and blocking reasons. `CanAuthorizeAction(const SessionRoots&, const RequiredEffects&) -> Expected<void>` consumes actual live prerequisites, never a report file as a key. Define SessionRoots/RequiredEffects DTOs in RecoveryReadiness.hpp using the root/footprint types from Task 2.

- [ ] Write `Assistant no protection RECOVERY_REQUIRED`, `Assistant disk full blocks write allows read`, `Assistant unsupported doc blocks`, `Assistant Git conflict blocks`, `Assistant fake passed report cannot enable`. Assert `targetAfter == targetBefore` for every denial.
- [ ] Run RED; qualify staged/unstaged/untracked/binary/dirty docs, cross-process contention, crash during apply/restore, storage retention and transcript deletion. Never evict pinned/active recovery points to enable writes.
- [ ] Run `python tools/assistant/qualify_recovery.py --project <disposable-project> --report logs/assistant-recovery/report.json` and native prefixes GREEN. Required failures/skips fail qualification; report each OS separately.
- [ ] **Milestone A gate:** no following task may expose assistant mutations if Source Control or required restoration fails.

### Task 8: Shared validated executor and effect gate

**Files:** Create editor/ControlExecutor.*, assistant/MutationCoordinator.*, tests/app/ControlExecutorTests.cpp and tests/assistant/MutationCoordinatorTests.cpp. Modify existing ControlMethods.hpp, ControlServer.*, ControlServerLayer.* and owning layer update.

**Interfaces:** ControlMethod adds internal resolver yielding the Task 2 MutationFootprint `{files, documents, sceneTargets, generatedOutputs, runtimeEffects}`. `ControlExecutor::Execute(method, args, const NativeCaller&, MethodContext&) -> Json`; `Enqueue(string method, Json args, NativeCaller caller) -> RequestId`; `Drain(MethodContext&)`. NativeCaller origin is host-owned, never decoded from arguments. MutationCoordinator executes acquire -> readiness -> capture/permit -> revalidate -> handler -> receipt.

- [ ] Write `ControlExecutor external validation compatible`, `ControlExecutor embedded ENet off`, `Assistant cannot forge origin`, `Assistant undeclared effects denied`, `Assistant changed precondition denied`, `Assistant permit cannot replay`, `Assistant outcome journal failure blocks next write`. Assert stable error codes and zero denied handler calls.
- [ ] Run RED; extract Dispatch validation/errors while preserving external wire schemas. Assistant adapters validate supported nested/enum/bounded arguments. Unknown schemas/effects stay hidden; no second handler registry.
- [ ] Audit mixed query/write methods and delayed jobs; mutates is currently only descriptive. Drain on game thread even with ENet off. Bound queues; no ECS access on inference/render threads. Assistant-only gate must not let worker impersonate a user-origin caller. Do not expose synthetic UI/key/mouse automation, arbitrary editor-command execution or settings/file routes that could click the engine toggle, commit or erase recovery; add a denied-UI-bypass test. Ordinary user UI remains available.
- [ ] Run ControlExecutor*/Assistant* GREEN and `python -m unittest discover -s tools/mcp -p "test_*.py" -v`. Keep subsequent chat milestones read-only despite the protected execution route existing.

### Task 9: Duplex worker and Unsloth capability proof

**Files:** Create assistant/WorkerProcess.*, WorkerProtocol.*, tests/assistant/WorkerProcessTests.cpp; tools/assistant/protocol.py, model_client.py, worker.py, probe.py, test_protocol.py and test_model_client.py.

**Interfaces:** WorkerProcess Start(WorkerLaunch), Send(Json), PollEvents(size_t max), Cancel(turnId), Close(stop_token). Protocol v1 newline-delimited records contain protocolVersion, sessionId, turnId, generation, sequence, kind. Commands: start_turn, tool_result, cancel, load_history, shutdown. Events: text_delta, tool_request, tool_result_recorded, diagnostic, terminal. Authority is never represented by a model-provided field.

- [ ] Test split UTF-8/SSE, incomplete/malformed tool JSON, duplicate IDs, huge records, early EOF, full stderr, cancellation, worker crash and grandchildren cleanup. Assert complete calls emitted once, partial calls never emitted, owned children exit and Unsloth survives.
- [ ] Run RED; implement editor-local duplex pipes following ProcessArguments.cpp ownership/quoting patterns. Its existing stdin is NUL/dev/null and cannot be reused unchanged. Drain stderr to logs, keep blocking I/O off frame/render threads, bound queues and PostEmptyEvent on new events.
- [ ] Python uses stdlib HTTP, loopback only, no redirects/environment proxy routing. Probe GET /v1/models plus streaming POST /v1/chat/completions using installed Unsloth/exact model. Disable server-side web/shell/Python tools; client-side calls only. Unsupported API/model mode blocks the feature with evidence, not prose-command execution.
- [ ] Run Python/native GREEN and `python tools/assistant/probe.py --endpoint <loopback-url> --model <exact-id> --report logs/assistant-model/probe.json`. Credentials only through masked session input or AETHERCORE_ASSISTANT_API_KEY, never argv. Demonstrate safe read-only tool round trip before editor tools.

### Task 10: Durable rich conversation history

**Files:** Create tools/assistant/history.py and test_history.py; extend worker/protocol and WorkerProtocolTests.

**Interfaces:** HistoryStore.open(project_root, conversation_id), append(event) -> sequence, load() -> list[Event], export(destination), delete_conversation(id). Event kinds: user, assistant_text, tool_request/result, source_reference, diff, attachment, diagnostic, verification, restore_reference. Native recovery journal owns checkpoint data; transcript references it.

- [ ] Test torn append, version mismatch, bounded paging, missing attachments, restart mid-turn, duplicates, redaction and deleting chat with pinned recovery. Assert no replay and pinned recovery remains usable independently.
- [ ] Run RED; use .aether/assistant/conversations/ logs and immutable attachment IDs. One writer per file; exact requests/results retained with credentials omitted. Summaries rebuild from events. Do not duplicate recovery blobs in chat storage.
- [ ] Run test_history.py GREEN; kill real worker mid-stream and reopen offline. Failed action evidence remains linked to native receipts, not silently marked successful.

### Task 11: Read-only context and agent session

**Files:** Create assistant/AssistantSession.*, AssistantFiles.* (read-only initially), tests/assistant/AssistantSessionTests.cpp; tools/assistant/context.py, session.py, test_context.py and test_session.py.

**Interfaces:** AssistantSession BeginTurn(text, SelectionContext) -> TurnId, HandleEvent(Json), Cancel(TurnId), OnProjectChanged(EditorProjectContext), SetMode(AssistantMode) (native user only). AssistantFiles Read(path, range), Search(root, query, limits) validate canonical read allowlists. No writer exists yet. Worker requests a selected catalogue through host, never directly invokes native handlers.

- [ ] Write `Assistant readonly refuses mutating methods`, `Assistant stale generation cannot cross project`, `Assistant ambiguous target clarifies`, `Assistant context preserves tool pairs`, and missing-graph fallback. Assert `targetWrites == 0` in all read-only scenarios.
- [ ] Run RED; assemble language/target/selection, source hashes, relevant SDK/examples, recent diagnostics and compact history. Limit tool groups and results; no repo-wide prompt dump or graph dependency. Refresh stale IDs after reload.
- [ ] Initial builder defaults: 24 tool actions, 2 repairs, 600-second turn limit, 64 KiB returned tool content and 1 MiB protocol record. Discover actual model context, reserve completion/tool headroom, conservatively count when token API is absent. Defaults are tunable user settings, not measured optimums.
- [ ] Run native/Python GREEN, then exact Qwen SDK-question/unsupported/ambiguous cases using current source. Text-model screenshot attachments are for the user, not model visual evidence.

### Task 12: Assistant panel and user-only settings

**Files:** Create debug/AssistantPanel.*; modify DebugLayer.*, relevant DebugPanel lifecycle, EngineSettings.hpp/.cpp, settings tests and SourceControl links.

**Interfaces:** Panel owns view/composer state and references AssistantSession; it renders text/code/source/diff/tool/error/attachment/verification blocks with checkpoint links. Reflect editor.assistant.* endpoint/model/Python/budget fields as User; mode is session-only. Credentials are not reflected/persisted settings.

- [ ] Add settings tests asserting SettingsHomeFor(key) == User and no export into project settings or published outputs. Test callback composition/project switch cancelling old sessions.
- [ ] Implement dockable rich history, composer, Stop, conversation create/open/export/delete, model status, context chips and source-control links. Match theme; OwnsEditingKeys protects typing from game shortcuts. Hide panel without losing session; close/project-switch cancels/joins owned work.
- [ ] Run settings/session GREEN and real owned-editor stream/scroll/reopen/hide/cancel/switch smoke. Record responsiveness/latency alongside game rendering; mocks cannot qualify interaction.
- [ ] **Milestone B:** read-only chat works, and model writes remain denied even after a previous session's engine mode.

### Task 13: Typed authored script-property updates

**Files:** Create editor/ScriptPropertyEdit.*, tests/app/ScriptPropertyEditTests.cpp; modify existing ControlMethods2D.cpp, ComponentDrawersScripts.cpp and debug/EditorCommand.*.

**Interfaces:** `SetAuthoredScriptProperty(ServiceContainer&, uint32_t entityId, string_view type, string_view property, const Json& value) -> Expected<void>` uses shared host metadata/conversion. Add typed SetScriptPropertyCommand. Catalogue operation script.set_property / tool set_script_property accepts id/type/property/value and updates an existing attachment.

- [ ] Write `Assistant setter updates existing field`, `Assistant serialized field overrides default`, `Assistant invalid field/type/duplicate attachment refuses`, `Assistant property undo preserves entity`. Exercise C#/Rust descriptors and serialized scene round-trip; no append/remove-readd trick.
- [ ] Run RED; extract non-UI property writer from Inspector, preserving UI semantics. Missing descriptors produce actionable build guidance. Supply authored document/version footprint and use MutationCoordinator; initial writes require Edit mode.
- [ ] Run GREEN plus real reference projects: set field, save/reopen, inspect actual value, undo and checkpoint-restore. Existing add_script behaviour is unchanged.

### Task 14: Protected project-authoring capabilities

**Files:** Extend AssistantFiles, MutationCoordinator and method effect adapters; create tests/assistant/AssistantAuthoringTests.cpp. Modify only relevant existing control-method files.

**Interfaces:** AssistantFiles Patch(FilePatchRequest) uses expected hash/bounded exact edits; Create(FileCreateRequest), Remove(FileRemoveRequest), Move(FileMoveRequest) require coordinator-owned permits. No model arbitrary shell or broad recursive deletion. Authoring capability catalogue contains only qualified file/document operations.

- [ ] Test traversal/junction escape, .git/recovery storage denial, late-discovered files, binary preimage, create/delete/rename, human save conflict, unknown dirty owner, full storage and wrong editor. Assert zero changes on denial and complete receipts on success.
- [ ] Implement script editing, entities/transforms/reflected components, existing model assignment, prefabs and supported material/atlas/animation/pixel authoring through existing owners. Admit each tool only after complete footprint and adapter tests. Hidden imports/bakes/settings/delayed writers remain unavailable until fully covered; arbitrary output paths cannot bypass preimage capture.
- [ ] Dirty documents stay authoritative; generated artifacts are distinguished from authored outputs. Select/import existing local assets; no automatic online asset downloads or generative media service.
- [ ] Run authoring/recovery GREEN; actual slow-player/wrong-model tasks persist through Stop/save/reopen. Restore with model disconnected; preserve prior user work/unrelated later edits. **Only this qualified capability set may now mutate.**

### Task 15: Async compile, bounded repair and behavioural checks

**Files:** Extend AssistantSession/worker session; create tools/assistant/gameplay_jobs.py, test_gameplay_bridge.py, tests/assistant/AssistantGameplayTests.cpp. Modify current async compile/Play integration only where needed; correct README/getting-started mid-Play F5 claims.

**Interfaces:** GameplayJobBridge start(request), status(job_id), cancel(job_id) wraps existing tools/project/gameplay_jobs.py. Native host validates project/ownership; only owned IDs are accepted. Session states: StopRequested, AuthoredResolved, Applying, Compiling, Verifying, Resuming, Completed/Failed/Cancelled. Define request/result DTOs in AssistantSession.hpp; terminal diagnostics and artifact references retain existing AgentReport semantics.

- [ ] Test C#/Rust fail -> repair -> pass, Stop remaps IDs, stale successful build rejected, compile cancellation and repair exhaustion. Assert failed builds never Play and reported success comes from terminal job evidence.
- [ ] Run RED; reuse asynchronous compile paths, not synchronous DoReload in the frame. Stop -> authored re-resolve -> edit -> build -> resume explicitly resets simulation. Use existing launchConfig/cases assertions for tests.
- [ ] Autonomous verification copies authored inputs to owned scratch projects excluding recovery/history/builds, reducing fixture/save pollution; this is not an OS sandbox. Interactive Play retains documented runtime side effects.
- [ ] Run GREEN and real fresh-script creation/attachment, compilation repair and assertions for both languages. Link reports/checkpoints; restore source/scene, rebuild and verify actual rollback.

### Task 16: Live preview with explicit authored persistence

**Files:** Extend ScriptPropertyEdit, AssistantSession and PlaySession coordination; create tests/assistant/AssistantPreviewTests.cpp. No init/shutdown reordering.

**Interfaces:** PreviewEdit(target, value) -> PreviewReceipt (transient); KeepPreview(receipt) -> Expected<AuthoredEditReceipt> stops Play, re-resolves authored identity, uses protected setter. Receipt includes project/scene/generation and original authored value, not a persistent runtime ID.

- [ ] Test preview disappears on Stop, kept value survives reopen, target replaced/destroyed, serialized/default precedence, unsupported kinds and later human conflict. Assert temporary changes never claim authored persistence.
- [ ] Run RED; expose only host-supported runtime field kinds. Script recompilation still resets simulation; show Temporary/Kept status.
- [ ] Run GREEN and real editor preview/keep/restore. A kept-value rollback restores authored data, not the former physics/heap state.

### Task 17: Engine-mode source changes and candidate build

**Files:** Create assistant/EngineDevelopment.*, tests/assistant/EngineDevelopmentTests.cpp; extend native scope policy, Source Control root selection and session UI.

**Interfaces:** EngineDevelopment InspectCheckout(Path) -> EngineContext, PlanBuild(context, preset, configuration) -> CandidateBuildPlan, BuildCandidate(plan, stop_token) -> CandidateBuildReport. Toggle is native-only, session-scoped; candidate output is an isolated CMake binary directory.

- [ ] Test engine denied in Project mode, missing checkout, nested roots, toggle-off queued actions, protected generated/dependency paths, unrelated staged/dirty files and failed candidate. Assert running editor executable/build output untouched and source changes have receipts.
- [ ] Run RED; use preset/toolchain/configuration as source of truth with isolated output. No Git/worktree mutation, CI trigger changes, hooks or third-party edits. Record source revision/hash, preset/config, candidate executable/CWD and checks.
- [ ] Serialize builds, recheck source versions and run targeted tests/appropriate existing gauntlet against candidate. Recovery remains mandatory; source compilation failure leaves current editor usable.
- [ ] Run GREEN and real small first-party change in explicitly owned scratch checkout/build. Restore source and prove current working editor unaffected. Do not repurpose unrelated dirty user work as a fixture.

### Task 18: Owned editor restart and fallback

**Files:** Create tools/assistant/editor_handoff.py, test_editor_handoff.py; extend EngineDevelopment and editor startup/session restore, reusing Start-AgentEditor ownership patterns.

**Interfaces:** HandoffReceipt v1 includes project, conversation, source checkpoint, old/candidate executable and CWD, process identity/start time, readiness nonce/deadline/state. handoff(receipt_path) survives closing editor; recover(receipt_path) acts only on recorded owned processes/builds.

- [ ] Test launch failure, crash-before-ready, wrong project/nonce, old process still running, cancelled unsaved prompt, timeout/helper crash and success. Assert same project/conversation fallback, Unsloth/unrelated processes unaffected.
- [ ] Run RED; user initiates Restart into build after checks; handle unsaved work and persist history first. Transfer helper ownership explicitly so normal editor shutdown does not kill the handoff.
- [ ] Use build-tree-root CWD, hidden/background launches in qualification, exact process identity and bounded readiness. Failure retains source checkpoints and last working build; it never becomes successful-update text.
- [ ] Run GREEN and actual owned-editor success/failure handoffs. Source rollback and executable fallback remain separate linked actions.

### Task 19: Offline/distribution/end-to-end qualification

**Files:** Create tools/assistant/qualify.py, test_qualification.py, docs/local-editor-assistant.md. Update MCP/API navigation, help, CMake staging and existing package-planning tests. Extend manual verification only.

**Interfaces:** `python tools/assistant/qualify.py --project <owned-fixture> --endpoint <loopback-url> --model <exact-id> --report logs/assistant/qualification.json`; credentials only session/environment. Report v1 contains phases/checks/skips/artifacts/process ownership/input hashes; required skips fail release qualification.

- [ ] Qualify recovery with model off, then exact Qwen chat, edits, C#/Rust repair, restore, preview, engine handoff, concurrent editors, interrupted history/restore, retention and missing artifacts. Verify no non-loopback inference or hidden server tools.
- [ ] CMake stages worker sources for Editor. Python/Git prerequisites appear in-panel; development-prerequisite mode is explicit. Qualify bundled runtime/license/update route before claiming self-contained installation; inference needs no Unsloth training packages in the editor worker.
- [ ] Explicitly exclude .aether/assistant/ from native/Web publication and test marker credential/history files never ship. Assert assistant/sourcecontrol sources absent from Launcher/GameRuntime build lists. Report Windows/Linux separately.
- [ ] Run Python, focused native and full CTest. Verify current gauntlet invocation, then run `./scripts/Run-DebugGauntlet.ps1 -Fast -ReportPath logs/assistant/native-gauntlet.json`. Run existing browser/publish checks where packaging changes apply. No new automatic workflows/hooks.
- [ ] Inspect terminal evidence, close owned processes, record measured latency/context/game responsiveness and review final diff. State unqualified items explicitly before release claims.

### Task 20: Local training dataset and measured adapter adoption

**Files:** Create tools/assistant/dataset.py, evaluate.py, train.py, associated test files and docs/assistant-training.md; extend conversation export/model selection only.

**Interfaces:** Dataset exporter joins transcript+native evidence by receipt ID and engine/tool-schema revision. It produces train/validation/held-out manifests. Evaluator runs identical owned tasks with tuned/untuned model IDs, same runner/quantization/context/tools/budgets. Training runner consumes explicit local configuration and writes base/recipe/adapter metadata.

- [ ] Test secret redaction, missing/failed verification exclusion from positive examples, disjoint task-family split, obsolete schema labels, clarification examples and training interruption. Assert held-out families disjoint, no secrets exported, absent evidence never labelled verified.
- [ ] Run RED; curate actual SDK/tool usage, errors, repairs and verified outcomes. Train the named Qwen2.5-Coder-7B-Instruct through installed Unsloth with a pinned/version-verified local recipe; choose parameters from measured setup. Training is an explicit separate job, never automatic during chat.
- [ ] Preserve untuned baseline. Compare tool validity, targets, task/test success, repairs, time/tokens and human corrections on unseen combinations/API changes. Lower loss or compile rate alone is insufficient.
- [ ] Run GREEN, then an explicitly requested real experiment. Record base revision, recipe, adapter, quantization and outcomes; qualify deployment compatibility and retain return to untuned model. Editor delivery does not depend on a fine-tuning win.

## Spec coverage and release gates

| Approved requirement | Tasks / evidence |
|---|---|
| Native Git and independent rollback before writes | 1-7; real Git/dirty-document/crash tests; native per-action gate in 8 |
| Rich persistent local chat and Unsloth connection | 9-12; actual model stream, restored history, disconnect/project-switch tests |
| Existing SDK/graphs/control actions reused | 8, 11, 13-15; current source references and external MCP regression |
| Scripts/scenes/materials/assets and iteration | 13-15; real language fixtures, correct footprints, restore with model off |
| Preview versus authored state | 16; Stop/reopen/keep and conflict tests |
| Engine toggle/build/restart/fallback | 17-18; isolated owned candidate and failed-start recovery |
| Offline packaging, portability, owned cancellation | 7, 9, 19; no history/keys shipped; each OS qualified separately |
| Local specialization with fair comparison | 20; disjoint held-out tasks and native verification evidence |

## Completion and execution handoff

- [ ] Check all spec requirements against this table and task evidence; source control/rollback cannot be replaced by a warning banner.
- [ ] Protection qualifies before any authoring registration. Every enabled method still checks native authority, readiness and durable recovery at invocation time.
- [ ] Verify new native core source ownership explicitly: Editor and tests as needed, never Launcher/GameRuntime. Keep ordinary scene recovery, typed undo and user Git staging intact.
- [ ] Retain user recovery points. Clean only owned scratch verification data/processes; never unrelated work/model installs.
- [ ] Review the saved plan and select execution approach before implementation. Recommended: subagent-driven execution with fresh review of recovery/durability, gate and every capability-enabling change.
- [ ] Commit/push is outside this planning task; follow the user's current Git authorization during future execution.
