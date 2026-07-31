# Publish Pipeline & Project Panel - Design

Date: 2026-07-31
Status: Approved. Implementation plan pending.

## Goal

Reduce publishing to one button that cannot produce a broken build, and split the
Project panel into two panels that each do one job.

Trigger: three separate defects shipped an empty world from a published Whisper build
in a single session (commits `27c4ea01`, `88871654`, `487a4ca5`). Each lived in a seam
created by the publish system having two of something - two stores for the startup
scene, two implementations of the packaging rules, two project-settings layers at
runtime. The fixes closed the individual holes; this closes the seams.

## Background: what is wrong today

`EditorProjectPublisher.cpp` is 1023 lines holding path helpers, copy helpers,
dev-artifact classification, verification, settings baking, report generation, pack and
publish orchestration. The TU is not in the `EngineTests` build at all, so none of it is
unit tested - every one of its rules is only exercised by running a real publish.

**The packaging rules exist twice, in two languages, and publish runs one over the other.**

| Rule | CMake | C++ |
|---|---|---|
| Prune dev files | `CMake/PrunePackageDevFiles.cmake` | `PrunePublishedDevFiles` |
| Verify package | `CMake/VerifyAppPackage.cmake` | `VerifyPublishedGame` |
| Bake `project.pak` | from `AETHERCORE_PROJECT_DIR` (a CMake cache variable - a *different* project than the editor has open) | from the open project |

Publish copies `PackageGame`'s output wholesale, then re-prunes, re-verifies and
re-bakes on top of it. The two rule sets can disagree, and the stale-runtime bug lived
in the seam: `PackageGame` produces a prebuilt `AetherGame.exe` that publish copies
without any staleness check beyond `PAK_PIPELINE_VERSION`, so runtime C++ fixes silently
did not reach published builds.

The project's own CMake comments already describe the simpler design:

> "Editor, GameRuntime and Launcher share one output directory - the 'editor bundle'
> the publisher ships wholesale"

with `add_dependencies(Editor GameRuntime)` guaranteeing "the runtime must be built and
co-located whenever the editor is". `usePackageTemplate = true` is what routes around it.

Further problems:

- **Correctness is optional.** `verifyOutput` is a user-facing checkbox. Turning it off
  skips the checks that catch broken packages.
- **Two divergent staging paths.** `usePackageTemplate` selects between copying the
  package template and `CopyRuntimeFromExecutableDir` + `CopyShippedDataPayload`, which
  produce *different payloads* - the latter copies only `engine.pak` and managed scripts.
- **Build-machine config in a committed file.** `[publish] outputroot =
  "D:\\AetherCore\\..."` is written into `ProjectSettings.toml`, an absolute local path
  in a file shared through git. Writing it is also what triggered the round-trip
  corruption that destroyed the file.
- **Publish mutates the editor's own data directory** through
  `syncEditorRuntimeProjectPak`.
- **Hand-tuned progress floats** (0.02, 0.08, 0.16, 0.48, 0.63, 0.70, 0.78, 0.84, 0.94,
  0.97, 1.0) that must be kept ordered by hand.
- **Ad-hoc error strings** built by concatenation, with no separation between what broke
  and what to do about it.

`ProjectPanel.cpp` is 961 lines and one class doing project settings, folder repair, a
scene browser, a C# debugger picker, pack, engine-pak rebuild, shader recompile and the
publish dialog - across three unrelated tabs (Overview / Assets / Build), with four
parallel `status`/`succeeded` string pairs and nine members mirroring
`EditorProjectPublishOptions`. Its scene table duplicates the Scenes list in
`HierarchyPanel`, which already owns the startup-scene star.

## Decisions

1. **Publish is one button with no options.** Every toggle is deleted, not relocated.
2. **The runtime payload comes from the editor's own bundle directory.** CMake already
   guarantees it is fresh, so the stale-runtime class of bug disappears by construction -
   no version check required.
3. **`PackageGame` and its two helper scripts are deleted.** Nothing else references
   them; this leaves exactly one implementation of the packaging rules, in C++, reachable
   from unit tests.
4. **A non-Release build publishes and is labelled.** Debug builds are useful for "send
   it to someone to test"; refusing would block the common case for a rule that only
   matters at ship time.

## Architecture

### Publish

`src/app/editor/publish/`:

| TU | Responsibility | Depends on |
|---|---|---|
| `PublishPlan` | Pure resolution: project + editor bundle dir + build config produce resolved paths, product name and config label. No IO. | filesystem paths only |
| `PublishSteps` | One function per step, uniform signature. The pipeline is an ordered list. | `PublishPlan`, io, asset pipeline |
| `PublishVerify` | Every package invariant, in one place. | io, pak backend |
| `PublishReport` | The human-readable report written beside the package. | io |
| `EditorProjectPublisher` | Thin orchestration: build plan, run steps, report progress. | all of the above |

**Step signature.** Each step is
`StepResult Step(const PublishPlan&, PublishContext&)` where `PublishContext` carries the
mutable bits (paths produced by earlier steps). Progress is `completedSteps / totalSteps`
- computed, never authored.

**Step list**, in order:

1. Validate project - open, descriptor present, startup scene set and backed by a scene
   file. (Reuses `app::ValidateProjectStartupScene`.)
2. Clean output directory.
3. Cook binaries and pack `project.pak`.
4. Stage runtime from the editor bundle - runtime executable, `nethost.dll`,
   `engine.pak`, shipped `EngineSettings.toml`, managed assemblies.
5. Bake `EngineSettings.toml` - fold the project layer in, force `autoplay = true`,
   re-validate the startup scene against what was actually packed.
6. Build project C# scripts, when the project has a `.csproj`.
7. Prune dev artifacts.
8. Verify.
9. Write the report.

**Failure shape.** `PublishFailure { std::string step; std::string message; std::string
remediation; }`. The UI shows the message and, separately, what to do. This replaces
error strings assembled by concatenation at the call site.

**Replaced:** `CopyRuntimeFromExecutableDir` and `CopyShippedDataPayload` are the two
divergent halves of today's staging; step 4 replaces both with one staging step that has a
single explicit payload list.

**Deleted:** `EditorProjectPublishOptions` (all eight fields), `FindCMakePackageDirectory`
and the `usePackageTemplate` branch it feeds, the `data/config/ProjectSettings.toml`
runtime lookup (nothing has ever written that file, so the branch has always been
unreachable), CMake `PackageGame`, `CMake/PrunePackageDevFiles.cmake`,
`CMake/VerifyAppPackage.cmake`, and the `AETHERCORE_PROJECT_DIR` cache variable.

**Derived, not configured:** output directory is
`<project root>/Builds/<Platform>/<Product>`; product name is the project name passed
through `SanitizePathSegment`; platform is the host platform.

**Moved, not deleted:** `syncEditorRuntimeProjectPak` copies `project.pak` into the
*editor's* data directory so the dev `AetherGame.exe` can run. That is a dev convenience,
not publishing, and publishing must not write outside its own output folder. Pack already
performs it; Pack keeps it and publish stops doing it.

**Replaced by actions:** `openAfter` becomes the Build panel's Open Folder button;
`cleanOutput` and `verifyOutput` become unconditional, because the alternatives are
"ship stale files" and "skip the checks".

### Panels

**`ProjectPanel`** keeps project identity, the folder table with Repair, and a
startup-scene **combo box** replacing the scene table that duplicates `HierarchyPanel`.
The star in the Hierarchy scene list remains the shortcut; both write
`ProjectSettings.toml` through `app::WriteProjectStartupScene`, so there is one store and
two entry points, not two stores.

**`BuildPanel`** is new and holds:

- A config banner stating the build configuration, e.g. *"Debug build - for testing, not
  for shipping"*.
- The action row: Pack, Recompile Shaders, Rebuild Engine Pak, Publish.
- The C# debugger installation row.
- One status area: last action, its result, and its remediation when it failed.
- Open Folder and Run Build, shown after a successful publish.
- An inline progress bar while publishing. No modal, no checkboxes.

State collapses from four `m_*Status` / `m_*Succeeded` pairs and nine publish-option
mirrors to a single
`ActionStatus { std::string message; bool ok; std::string remediation; std::filesystem::path output; }`.

## Data flow

```
ProjectSettings.toml  ──[app.startupScene]──┐
                                            ├─> PublishPlan ─> steps ─> package dir
editor bundle dir ──[runtime + engine.pak]──┘                            │
                                                                         v
                                              data/config/EngineSettings.toml
                                              (shipped layer, startup scene baked,
                                               autoplay forced true)
```

At runtime the published package resolves no project settings layer at all
(`app::ResolveRuntimeProjectSettings`), so the baked file is the whole truth.

## Error handling

- Validation failures stop before any output is written, so a failed publish never leaves
  a half-built package behind.
- Every failure carries remediation text. Examples: no startup scene set → "Pick one in
  the Project panel or the star in the Scenes list"; startup scene missing from the pack →
  "The scene file was not found in the project".
- Verification is a hard gate. A package that fails verification is reported as a failed
  publish, not a warning.

## Testing

New unit tests:

- **`PublishPlan`**: output path derivation, product-name sanitisation (spaces, path
  separators, empty name fallback), config labelling.
- **`PublishVerify`** against synthesised package directories: missing required file,
  dev artifact present, startup scene absent from the pak, `autoplay` false, no shaders in
  `engine.pak`. These invariants currently only run against a real 12 MB build, which is
  why regressions in them reach the user instead of CI.

Existing `ProjectStartupSceneTests` and `TomlConfigRoundTripTests` stay as they are.

Manual verification: publish Whisper, confirm the package boots its Title scene, and
confirm the report records the build configuration.

## Sequencing

Publish lands first, panels second. The Build panel's surface - what it shows, what
progress it reports, what a failure looks like - is determined by the publish API, so
building it against today's API would mean rewriting it immediately. Each stage ends with
the editor building and the suite green.

## Consequences

- **The only headless packaging path is removed.** Building a redistributable without
  opening the editor is no longer possible. If CI later needs it, it would be rebuilt on
  the C++ side against the same rules rather than as a second implementation.
- **`[publish]` disappears from `ProjectSettings.toml`.** It is stripped silently when
  present, so existing project files heal on the next write rather than erroring.
- **`Builds/Pack/` and `Builds/<Platform>/` both remain** under the project root.
  Retargeting the output layout is out of scope.

## Out of scope

- Multiple named build targets or shared build configurations.
- Cross-platform publishing from one host.
- Any change to the pak format or the asset pipeline.
- The Hierarchy panel's scene list, beyond it remaining the startup-scene shortcut.
