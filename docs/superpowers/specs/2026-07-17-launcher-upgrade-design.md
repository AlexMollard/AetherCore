# Launcher Upgrade — Design

Date: 2026-07-17
Status: Approved for planning

## Problem

The project launcher (`ProjectLauncherWindow` + `LauncherLayer`) has two usability
gaps:

1. **You cannot remove a project from the recents list.** `LauncherLayer` has
   `RememberRecent()` but no removal counterpart; the `ProjectLauncherWindowActions`
   struct exposes open/create/browse/close/save but nothing to forget a project;
   and cards are click-to-open only. A moved or deleted project shows a red
   `MISSING` tag (`ProjectLauncherWindow.cpp:147`) and a tooltip, but there is no
   way to act on it — it is stuck on the list forever.

2. **The Open and Create dialogs are confusing.**
   - **Open** (`ProjectLauncherWindow.cpp:292`) asks for a raw path to
     `ProjectSettings.toml`, even though the backend `ResolveProjectRoot()`
     already accepts a plain folder and strips the filename itself. It demands
     more precision than it needs and offers no feedback about whether a project
     is actually there.
   - **Create** (`LauncherLayer.cpp:501`) writes the project **directly into the
     chosen folder** (folder == project root), not `<parent>/<name>/`. Name and
     folder are decoupled, there is no preview of where the project lands, no
     inline validation (errors only appear after pressing Create), and the
     "empty folder" expectation is buried in one line of grey text.

## Goals

- Remove a project from recents (present or missing), plus locate a moved
  project and reveal a project's folder.
- A Create flow that matches Unity/Godot/VS: pick a parent location + name, see a
  live preview of the final path, and get inline validation before committing.
- An Open flow that accepts a folder (not just a `.toml`) and confirms inline
  whether a project is present.
- Preserve the cross-platform contract (Windows + Linux).

## Non-goals

- No redesign of the launcher's visual identity / layout (header, grid, theme
  stay as-is).
- No project-management features beyond recents (no templates gallery, no cloud,
  no project settings editing here).
- No change to how the editor is spawned once a project is chosen.

## API surface

`ProjectLauncherWindowActions` (in `ProjectLauncherWindow.hpp`) gains three
callbacks, and `createProject` changes shape:

```cpp
// New:
std::function<void(std::filesystem::path)> removeRecent;      // forget a project (present or missing)
std::function<void(std::filesystem::path)> revealProjectFolder; // open folder in OS file manager
std::function<void(std::filesystem::path)> relocateRecent;    // re-point a moved project

// Changed: parentDir now (not the final root); the layer composes the subfolder.
std::function<void(std::filesystem::path /*parentDir*/, std::string_view /*name*/, project::ProjectTemplate)> createProject;
```

Two shared pure helpers in the `project::` namespace keep the dialog preview and
the layer's creation in agreement:

```cpp
// Sanitize a display name into a safe folder name: trim ends, strip illegal
// filename chars + control chars, collapse internal whitespace to single spaces.
// Returns "" when nothing usable remains (so callers can treat it as invalid and
// disable Create — the fallback name is NOT baked in here).
std::string SanitizeProjectFolderName(std::string_view name);

// Compose the final project root: parent / SanitizeProjectFolderName(name).
// Empty parent or empty sanitized name -> empty path (caller treats as invalid).
std::filesystem::path ComposeNewProjectRoot(const std::filesystem::path& parent, std::string_view name);
```

## Recents remove UX (`DrawProjectCard` / `PaintRecentsGrid`)

- **Hover ×**: a small × button in the card's top-right corner, shown on hover.
  Clicking it calls `removeRecent`. Its hit region is tested **before** the
  card's open-press so removing never also opens the project. (Implementation: a
  dedicated `InvisibleButton`/hit-test over the corner rect, evaluated before the
  full-card `InvisibleButton` result is honoured.)
- **Right-click context menu** (`BeginPopupContextItem` on the card):
  - Present project: *Open* · *Reveal folder* · *Remove from list*.
  - Missing project: *Locate moved project…* · *Remove from list*.

The existing `MISSING` tag and tooltip are retained.

## Create dialog — friendly

Fields: **Template** (combo, unchanged) · **Name** · **Parent location** +
browse button (folder picker).

- **Live preview line**: `Creates: <parent>/<sanitized-name>` using
  `ComposeNewProjectRoot`, updated every frame.
- **Inline validation** (shown as you type, not only on submit):
  - name empty -> "Enter a project name.";
  - parent empty -> "Choose a location.";
  - sanitized name empty (all illegal chars) -> "Name has no usable characters.";
  - target already exists and is non-empty -> warning "A non-empty folder already
    exists here — files may be overwritten." (warning, not a hard block).
- **Create** button is disabled until name and parent are non-empty and the
  composed root is non-empty.
- On submit, the view calls `createProject(parentDir, name, template)`; the layer
  composes the root via `ComposeNewProjectRoot` and proceeds as today
  (`WriteProjectDescriptor` -> `RememberRecent` -> `SpawnEditorFor`).

## Open dialog — friendly

- Becomes **folder-based**: label "Project folder"; the browse button opens a
  **folder** picker (`PickProjectFolder`). The text field still accepts either a
  folder or a `.toml` path (`ResolveProjectRoot` strips the filename).
- **Inline status** as the path changes: `✓ Project found` when
  `HasProjectDescriptor(ResolveProjectRoot(text))` is true, else
  `✗ No ProjectSettings.toml here` (neutral/grey when the field is empty).
- **Open** is disabled until the entered path resolves to a folder containing a
  descriptor.

## Layer + persistence (`LauncherLayer.cpp`)

- `RemoveRecent(root)`: `std::erase_if` on `m_recentProjects` by normalized path,
  then `PersistSettings()` (which calls `SaveRecentProjects`).
- `RelocateRecent(oldRoot)`: open a folder picker; if the chosen folder has a
  descriptor, `RemoveRecent(oldRoot)` then `RememberRecent(newRoot)` (which
  moves it to the front and persists). If not, set `state.error`.
- `RevealProjectFolder(root)`: open the folder in the OS file manager via a small
  platform helper — `ShellExecuteW(nullptr, L"open", folder, ...)` on Windows,
  `xdg-open` on Linux (guarded by `#ifdef _WIN32` / else, matching existing
  platform splits in `ProjectCommon.cpp`).
- `CreateProject` handler updated to take the parent dir and compose the root.

## Testing

- **Unit (doctest)** — new pure helpers, no UI:
  - `SanitizeProjectFolderName`: trims, strips illegal filename chars
    (`\\ / : * ? " < > |`), collapses internal whitespace, empty/all-illegal ->
    `"AetherProject"`.
  - `ComposeNewProjectRoot`: `parent / sanitized`; empty parent or empty
    sanitized -> empty path.
- **Runtime** — build + launch the `Launcher`, screenshot:
  - the hub with a seeded **missing** recent, confirm the hover × and
    right-click menu appear and removal drops the card;
  - the redesigned **Create** dialog showing the live `Creates:` preview and a
    disabled Create button until valid;
  - the redesigned **Open** dialog showing the inline found/not-found status.

## Risks

- **Card hit-testing**: layering a corner × hit region over the full-card
  `InvisibleButton` must be ordered so the × wins and suppresses the open. The
  context menu (`BeginPopupContextItem`) must bind to the card item id. Mitigation:
  evaluate the × rect first and gate the open-press on `!removedThisFrame &&
  !xHovered`.
- **createProject signature change** touches the view, the `Actions` struct, and
  `LauncherLayer`; the MCP control methods that call into the launcher
  (`BuildLauncherControlMethods`) must be checked for any create/open coupling.
- **Reveal-in-folder** adds a small amount of platform code; kept minimal and
  behind the existing `_WIN32` split.
