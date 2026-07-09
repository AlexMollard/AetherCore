# Settings & Config Consolidation - Design

Date: 2026-07-10
Status: Approved. Implementation plan pending.

## Goal

Collapse a confusing sprawl of `.toml` config files into a clear, role-named set.
The user's mental model: there should be exactly **two files you author** - one for
the engine, one per project - and everything else should be clearly-labelled local
machine state that never enters the repo.

Trigger: the recurring "lost all my settings" corruption (a duplicate-key parse
error in `debug.toml`, fixed separately) exposed how many overlapping config files
exist and how unclear their roles are.

## Audit of the current state (what exists today)

**Authored settings:**

| File | Role | Actually consumed? |
|---|---|---|
| `resources/config/engine.toml` | Layer-2 shipped engine defaults; CMake deploys to `data/config/engine.toml` beside exe | Yes, fully |
| `resources/settings/engine.toml` | Template copied into new projects | Only as a copy source ("only app.startupScene used") |
| `projects/<P>/settings/engine.toml` | Per-project settings | Only `app.startupscene` is read; window/graphics keys are DEAD |
| `projects/<P>/.project/aether.project` | Project descriptor: `[project]` name/version, `[paths]` | Yes (discovery + path resolution) |
| `projects/<P>/.project/publish.toml` | Publish/packaging settings | Yes (publish dialog) |

**Runtime/local state (generated, not authored):**

| File | Location | Role |
|---|---|---|
| `settings.toml` | `LocalAppData/AetherCore/` | Layer-3 per-user overrides (delta vs base) - the only file the running app writes |
| `debug.toml` | `data/config/` beside exe | Editor/DebugLayer UI state (panels, gizmos, recent projects, layout) |
| `layouts/*.toml` | `LocalAppData/AetherCore/layouts/` | Named editor layout presets |

**Not settings (content/assets - out of scope, untouched):** `*.scene.toml`,
`*.prefab.toml`, `assets/materials/*.toml` + `properties.toml`.

The confusion has three concrete causes: (a) two same-named `engine.toml` files in
`resources/`; (b) the project `engine.toml` carries a full but mostly-dead settings
dump; (c) nothing is named for its role.

## Decisions (locked with the user)

- **Scope:** 2 authored files + clean local state. User-overrides and editor state
  stay as local machine artifacts in LocalAppData, out of the repo.
- **Project file is a real cascade layer:** cascade becomes
  defaults -> shipped -> **project** -> user. The project's `[window]`/`[graphics]`/
  `[app]` keys become live overrides (they are dead today).
- **`ProjectSettings.toml` at project root IS the project file.** It absorbs
  `aether.project` (`[project]` + `[paths]`), `settings/engine.toml`
  (`[app]`/`[window]`/`[graphics]`), and `publish.toml` (`[publish]`). The
  `.project/` and `settings/` directories disappear.
- **Launcher opens a `ProjectSettings.toml` file** (file picker) rather than a
  folder. Project root = the file's parent directory. Sets up future project
  expansion.
- **Hard rename, no runtime migration fallback.** No migration-shim code. The
  repo's sample `TestingProject` is converted by hand as part of this change. Local
  LocalAppData state (user settings, editor layout, recent list) resets to defaults
  on first run - accepted.
- **Editor state moves out of the build tree** into LocalAppData.

## Target file set

**Authored (2 files):**

1. **`EngineSettings.toml`** - engine-wide shipped defaults.
   - Source: `resources/config/EngineSettings.toml`
   - Deployed by CMake to `data/config/EngineSettings.toml` beside the exe (the
     `config://` mount). Read-only at runtime.
   - Content: full `[window]`, `[graphics]`, `[app]` default set (unchanged schema).

2. **`ProjectSettings.toml`** - one per project, at the project **root**. The
   project file the launcher opens. Structure:

   ```toml
   [project]
   version = 1
   name = "Testing Project"

   [paths]
   assets  = "assets"
   scenes  = "scenes"
   prefabs = "assets/prefabs"
   scripts = "scripts"

   [app]
   startupScene = "Testing"
   # autoplay etc.

   [window]      # optional project-level engine overrides (layer 3)
   [graphics]

   [publish]
   platformName = "Windows"
   productName  = "TestingProject"
   # buildScripts, cleanOutput, openAfter, outputRoot, syncEditorPak,
   # usePackageTemplate, verifyOutput
   ```

   New-project template: `resources/templates/ProjectSettings.toml`.

**Local machine state (LocalAppData/AetherCore, generated, never in repo):**

- `UserSettings.toml` (was `settings.toml`) - per-user delta vs layers 1+2+3.
- `EditorState.toml` (was `debug.toml`, **moved** from beside-exe to LocalAppData) -
  editor/DebugLayer UI state; recent-projects list now stores paths to
  `ProjectSettings.toml` files.
- `layouts/*.toml` - unchanged.

## The cascade (low -> high priority)

```
1. compiled-in EngineSettings defaults (C++ struct)
2. EngineSettings.toml     (shipped, read-only)
3. ProjectSettings.toml    (project - NEW live layer: [window]/[graphics]/[app])
4. UserSettings.toml       (per-user, runtime-written delta vs 1+2+3)
```

- `LoadLayered` gains a project layer between shipped and user. `base` (used for
  the user delta-save) = layers 1+2+3, so the user file stores only what the user
  changed beyond the project's effective values.
- Opening a project re-derives the cascade: `EditorProjectManager` re-runs the
  layered load with the project's `[app]`/`[window]`/`[graphics]` values, updating
  both `SettingsService::Values()` and its `base`. (Today only `startupScene` is
  pulled - `RefreshServices`.)
- `[project]`, `[paths]`, and `[publish]` are project-descriptor/editor concerns,
  NOT part of the engine-settings struct; they are read separately by the editor.

## Who writes what

- `EngineSettings.toml` - hand-authored / engine devs. Never written at runtime.
- `ProjectSettings.toml` - written on project creation, and by the publish dialog
  (`[publish]` section) and any project-settings editor UI. `[project]`/`[paths]`
  written on create. Runtime engine Settings panel does NOT write here.
- `UserSettings.toml` - written by `SettingsService::Save()` (runtime Settings
  panel) as a delta.
- `EditorState.toml` - written by `DebugLayer` (SaveIfDirty).

## File-by-file change surface

**Engine settings core**
- `src/engine/utils/EngineSettings.hpp/.cpp`: default filenames
  `engine.toml` -> `EngineSettings.toml`, `settings.toml` -> `UserSettings.toml`.
  `LoadLayered` takes/loads a project layer; `base` includes it.
- `src/engine/AetherCore.hpp`: `settingsFile` default -> `EngineSettings.toml`.
- `src/engine/utils/SettingsService.cpp`: `Save()` delta base now includes project
  layer (mechanically unchanged if `base` is set correctly at load).

**Editor state relocation**
- `src/app/layers/DebugLayer.cpp`: resolve an explicit
  `GetUserConfigDir()/EditorState.toml` path and load/save via a path-based
  `TomlConfig` entry point (add a path overload or use `Load`/`Save` with an
  `ifstream`/`ofstream`). Do NOT route this through `TomlConfig::LoadFile`/
  `SaveFile`, so `TomlConfig`'s `config://`/`ResolvePath` semantics for engine
  files stay untouched. This is the single file that moves out of the build tree.

**Project system**
- `src/app/debug/EditorProjectManager.cpp`:
  - Descriptor read/write: `ProjectSettings.toml` at root with `[project]`,
    `[paths]`, `[app]`, `[window]`, `[graphics]`, `[publish]` - replacing
    `aether.project`.
  - Discovery: a folder is a project iff it contains `ProjectSettings.toml`.
  - `OpenProject` accepts the file path (root = parent dir).
  - Recent-projects entries store the `ProjectSettings.toml` path.
  - Feed the full project layer into the settings cascade (not just startupScene).
  - New-project creation writes `ProjectSettings.toml` from
    `resources/templates/ProjectSettings.toml`.
- `src/app/editor/EditorProjectContext.hpp`: drop `settingsDir`; paths resolved
  from `[paths]`. Add project-file path if needed.
- `src/app/debug/ProjectLauncherWindow.cpp`: `browseFolder` -> file picker for
  `ProjectSettings.toml` (new `browseProjectFile` action).
- `src/app/debug/ProjectPanel.cpp`: settings-file path and publish read/write ->
  `ProjectSettings.toml`; folder rows adjust (no more `settings/`, `.project/`).
- `src/app/editor/EditorProjectPublisher.cpp`: engine deploy path
  `engine.toml` -> `EngineSettings.toml`; read publish config from
  `ProjectSettings.toml [publish]`.

**Build / packaging**
- `src/app/CMakeLists.txt`: deploy `resources/config/EngineSettings.toml` ->
  `data/config/EngineSettings.toml`; `AETHER_DEFAULT_SETTINGS_DIR` /template ->
  `resources/templates/ProjectSettings.toml`.
- `cmake/VerifyAppPackage.cmake`: `data/config/engine.toml` ->
  `data/config/EngineSettings.toml`.

**Repo file changes (done by hand as part of the change)**
- Rename `resources/config/engine.toml` -> `resources/config/EngineSettings.toml`.
- Delete `resources/settings/engine.toml`; create
  `resources/templates/ProjectSettings.toml`.
- Convert `projects/TestingProject`: create root `ProjectSettings.toml` (merging
  `aether.project` + `settings/engine.toml` + `.project/publish.toml`); delete
  `.project/` and `settings/`.

**Tests**
- `tests/utils/EngineSettingsTests.cpp`, `tests/utils/SettingsServiceTests.cpp`:
  update filenames; add coverage for the 4-layer cascade (project layer overrides
  shipped, user delta computed against project baseline).

## Testing strategy

- Unit: engine settings cascade with a project layer - project overrides shipped
  default; user override wins over project; user delta serialized against the
  project-inclusive base; missing project file -> falls back to shipped+user.
- Unit: `TomlConfig` round-trip already covered (top-level key ordering regression).
- Manual/editor: open `TestingProject` via file picker; verify startup scene,
  window/graphics honoring project values; change a setting -> `UserSettings.toml`
  written in LocalAppData; publish writes `[publish]` back into
  `ProjectSettings.toml`; `EditorState.toml` appears in LocalAppData, not the build
  tree.

## Risks / edge cases

- **Editor-state relocation**: existing beside-exe `debug.toml` is orphaned
  (ignored). Acceptable per hard-rename decision; note it so the old file can be
  deleted.
- **`base` correctness**: the user delta-save is only correct if `base` includes
  the project layer at the moment of load AND is refreshed when a project is opened
  mid-session. Opening a project must rebuild both `values` and `base`.
- **Path resolution for `[paths]`**: must stay relative to the project root (the
  file's parent), matching today's `ResolveProjectPath` semantics.
- **Discovery back-compat**: none - folders without `ProjectSettings.toml` are no
  longer projects. The sample project must be converted in the same change so the
  editor still opens it.
- **Two writers of `ProjectSettings.toml`** (create + publish dialog): writes must
  be whole-file and preserve unrelated sections (don't clobber `[app]` when saving
  `[publish]`).

## Out of scope

- Content/asset TOML (scenes, prefabs, materials).
- A dedicated project-settings editor UI for `[window]`/`[graphics]` (hand-edit for
  now; the layer is wired so a UI can be added later).
- Runtime migration of pre-existing local files (explicitly declined).
