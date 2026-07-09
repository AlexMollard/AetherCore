# Settings & Config Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consolidate the `.toml` config sprawl into two authored files — `EngineSettings.toml` (shipped, engine-wide) and `ProjectSettings.toml` (one per project, the file you open) — with a 4-layer cascade, and move editor/user machine-state into clearly-named files under LocalAppData.

**Architecture:** Three independently-landable phases. Phase 1 renames the engine settings files and inserts a project layer into the settings cascade (defaults → shipped → project → user). Phase 2 relocates editor/debug state out of the build tree into LocalAppData. Phase 3 makes `ProjectSettings.toml` at the project root the single project descriptor (absorbing `aether.project`, `settings/engine.toml`, and `publish.toml`) and switches the launcher to open that file. Each phase ends with a green build and all tests passing.

**Tech Stack:** C++23, CMake (MSVC via `build/` dir, RelWithDebInfo), doctest (`tests/EngineTests`), toml++ (parse), custom `TomlConfig`/`EngineSettingsIO` serializers.

**Spec:** `docs/superpowers/specs/2026-07-10-settings-config-consolidation-design.md`

**Reference — build & test commands (used throughout):**
- Build engine tests: `cmake --build build --target EngineTests --config RelWithDebInfo`
- Run all tests: `./build/tests/RelWithDebInfo/EngineTests.exe`
- Run one case: `./build/tests/RelWithDebInfo/EngineTests.exe --test-case="<name>"`
- Build app: `cmake --build build --target App --config RelWithDebInfo`
- Run all bash commands from repo root `D:/AetherCore` (use absolute `cmake --build build ...`; do not `cd` into subdirs, it persists).

**Precondition:** The serializer bugfix (`TomlConfig.cpp`, `RenderGraphPanel.cpp`, `TomlConfigTests.cpp`, `tests/CMakeLists.txt`) is currently uncommitted in the working tree. Commit it before starting (its own commit), or it will be entangled with Phase 1's commits.

---

## Phase 1 — Engine settings: rename + project cascade layer

**File Structure (Phase 1):**
- Modify: `src/engine/utils/EngineSettings.hpp` — default filenames, header docs; `LoadLayered`/`LoadOrCreate` gain a project-file parameter.
- Modify: `src/engine/utils/EngineSettings.cpp` — `LoadLayered` applies a project layer between shipped and user; `base` includes it; comment fixups.
- Modify: `src/engine/AetherCore.hpp` — `Config::settingsFile` default.
- Modify: `src/app/Application.cpp:69` — pass-through (verify call site still compiles).
- Rename: `resources/config/engine.toml` → `resources/config/EngineSettings.toml`.
- Modify: `src/app/CMakeLists.txt` — deploy `EngineSettings.toml`.
- Modify: `cmake/VerifyAppPackage.cmake` — packaged-path check.
- Test: `tests/utils/EngineSettingsTests.cpp` — add project-layer cascade coverage.

### Task 1.1: Add the project layer to the cascade (test first)

**Files:**
- Test: `tests/utils/EngineSettingsTests.cpp`
- Modify: `src/engine/utils/EngineSettings.hpp:98-102`, `src/engine/utils/EngineSettings.cpp:225-266`

- [ ] **Step 1: Write the failing test**

Add to `tests/utils/EngineSettingsTests.cpp`:

```cpp
TEST_CASE("Four-layer cascade: project overrides shipped, user overrides project") {
    // Simulates LoadLayered's merge order using Apply (the same primitive it uses).
    EngineSettings s;                                              // 1: compiled defaults (vsync=true)
    EngineSettingsIO::Apply("[graphics]\nvsync = false\n", s);     // 2: shipped
    EngineSettingsIO::Apply("[window]\nwidth = 1600\n", s);        // 3: project
    EngineSettings base = s;                                       // base = layers 1+2+3
    EngineSettingsIO::Apply("[window]\nwidth = 2560\n", s);        // 4: user

    CHECK(s.graphics.vsync == false);   // shipped still in effect
    CHECK(s.window.width == 2560);      // user wins over project

    // User delta is computed against the project-inclusive base: only width differs.
    const std::string delta = EngineSettingsIO::SerializeOverrides(s, base);
    CHECK(delta.find("width = 2560") != std::string::npos);
    CHECK(delta.find("vsync") == std::string::npos);   // project/shipped values are NOT re-emitted
}
```

- [ ] **Step 2: Run it — it passes already (documents intent) OR compiles green**

Run: `cmake --build build --target EngineTests --config RelWithDebInfo && ./build/tests/RelWithDebInfo/EngineTests.exe --test-case="Four-layer cascade*"`
Expected: PASS. (This test uses only existing `Apply`/`SerializeOverrides`; it locks in the merge semantics the code change below must preserve.)

- [ ] **Step 3: Add the project-file parameter to the loader signatures**

In `src/engine/utils/EngineSettings.hpp`, change the two declarations (currently lines ~98 and ~102):

```cpp
        [[nodiscard]] static LoadedEngineSettings LoadLayered(std::string_view shippedFile = "EngineSettings.toml",
                                                              const std::filesystem::path& projectFile = {},
                                                              std::string_view userFile = "UserSettings.toml");

        [[nodiscard]] static EngineSettings LoadOrCreate(std::string_view shippedFile = "EngineSettings.toml",
                                                         const std::filesystem::path& projectFile = {},
                                                         std::string_view userFile = "UserSettings.toml");
```

- [ ] **Step 4: Implement the project layer in `LoadLayered`**

In `src/engine/utils/EngineSettings.cpp`, replace the body between the shipped-load block and the user-load block so the order is shipped → project → (set base) → user. Concretely, update the signature and insert the project layer BEFORE `result.base = result.values;`:

```cpp
    LoadedEngineSettings EngineSettingsIO::LoadLayered(std::string_view shippedFile,
                                                       const std::filesystem::path& projectFile,
                                                       std::string_view userFile)
    {
        AE_PROFILE_ZONE();
        LoadedEngineSettings result; // layer 1: compiled-in defaults

        // Layer 2: shipped project defaults (read-only, beside the executable).
        const auto shippedPath = ResolvePath(shippedFile);
        if (auto text = io::file_util::ReadText(shippedPath))
        {
            Apply(*text, result.values);
            AE_INFO(LogCategory::Engine, "Shipped settings loaded from {}", shippedPath.string());
        }
        else
        {
            AE_INFO(LogCategory::Engine, "No shipped settings file at {}; using compiled-in defaults.", shippedPath.string());
        }

        // Layer 3: per-project overrides (the open project's ProjectSettings.toml,
        // passed by the editor; empty in headless/engine-only runs).
        if (!projectFile.empty())
        {
            if (auto text = io::file_util::ReadText(projectFile))
            {
                Apply(*text, result.values);
                AE_INFO(LogCategory::Engine, "Project settings loaded from {}", projectFile.string());
            }
        }

        Sanitize(result.values);
        result.base = result.values; // base = layers 1 + 2 + 3

        // Layer 4: per-user overrides (writable, OS user-config dir).
        if (const auto userDir = io::PlatformPaths::GetUserConfigDir(); !userDir.empty())
        {
            const auto userPath = userDir / userFile;
            if (auto text = io::file_util::ReadText(userPath))
            {
                Apply(*text, result.values);
                AE_INFO(LogCategory::Engine, "User settings overrides loaded from {}", userPath.string());
            }
        }
        Sanitize(result.values);

        const auto& v = result.values;
        AE_INFO(LogCategory::Engine,
                "Settings resolved ({}x{}, VSync={}, FXAA={}, AsyncCompute={}, TargetFPS={})",
                v.window.width, v.window.height,
                v.graphics.vsync ? "on" : "off", v.graphics.fxaa ? "on" : "off",
                v.graphics.asyncCompute ? "on" : "off", v.app.targetFps);
        return result;
    }
```

And update `LoadOrCreate`:

```cpp
    EngineSettings EngineSettingsIO::LoadOrCreate(std::string_view shippedFile,
                                                  const std::filesystem::path& projectFile,
                                                  std::string_view userFile)
    {
        return LoadLayered(shippedFile, projectFile, userFile).values;
    }
```

`SaveUserOverrides`'s `userFile` default also becomes `"UserSettings.toml"` — update its declaration in the header (line ~107) and definition (line ~273) default arg.

- [ ] **Step 5: Fix the two callers that used the old 2-arg form**

`src/app/Application.cpp:69` and `src/engine/AetherCore.cpp:54` call `LoadLayered(config.settingsFile)` / `LoadOrCreate(config.settingsFile)`. These still compile (new params defaulted). Verify no positional-arg breakage by building in Step 6. No edit needed unless the build flags a mismatch.

- [ ] **Step 6: Build and run the full suite**

Run: `cmake --build build --target EngineTests --config RelWithDebInfo && ./build/tests/RelWithDebInfo/EngineTests.exe`
Expected: PASS, 130 test cases (129 + the new one).

- [ ] **Step 7: Commit**

```bash
git add tests/utils/EngineSettingsTests.cpp src/engine/utils/EngineSettings.hpp src/engine/utils/EngineSettings.cpp
git commit -m "Add project override layer to engine settings cascade"
```

### Task 1.2: Rename the shipped/user filenames end-to-end (rename + deploy together)

> Order matters: the CMake copy command references the shipped file by name, so the
> rename and the CMake deploy edit MUST land in the same task or the App build breaks
> with "source file does not exist".

**Files:**
- Rename: `resources/config/engine.toml` → `resources/config/EngineSettings.toml`
- Modify: `src/engine/AetherCore.hpp:39`
- Modify: `src/engine/utils/EngineSettings.cpp:162` (comment), `src/engine/utils/EngineSettings.hpp:83-92` (doc comment)
- Modify: `src/app/CMakeLists.txt:196-203` (deploy custom command — runs on every App build)

- [ ] **Step 1: Rename the shipped source file**

```bash
git mv resources/config/engine.toml resources/config/EngineSettings.toml
```

- [ ] **Step 2: Update the deploy custom command in the same breath**

In `src/app/CMakeLists.txt`, in the block that copies the shipped config (around lines 196–203), change both the build-tree copy and the beside-exe copy source/destination from `engine.toml` to `EngineSettings.toml`:
```cmake
            "${CMAKE_SOURCE_DIR}/resources/config/EngineSettings.toml"
            "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
        ...
            "${CMAKE_SOURCE_DIR}/resources/config/EngineSettings.toml"
            "$<TARGET_FILE_DIR:${target_name}>/data/config/EngineSettings.toml"
```

- [ ] **Step 3: Update the engine default filename**

`src/engine/AetherCore.hpp:39`, change:
```cpp
		const char* settingsFile = "EngineSettings.toml";
```

- [ ] **Step 4: Update doc comments referencing old names**

`src/engine/utils/EngineSettings.cpp:162`, change the override header string:
```cpp
		out << "# Overrides layered on top of the shipped EngineSettings.toml; only changed keys are stored.\n";
```
`src/engine/utils/EngineSettings.hpp:85-88`, update the cascade comment to name `EngineSettings.toml` (layer 2), `ProjectSettings.toml` (layer 3), `UserSettings.toml` (layer 4).

- [ ] **Step 5: Update the shipped file's own header comment**

Edit `resources/config/EngineSettings.toml`: replace the body comment that says "config://engine.toml" / "settings.toml" with `EngineSettings.toml` / `UserSettings.toml` so the file documents itself correctly. Keep all `[window]`/`[graphics]`/`[app]` key lines unchanged.

- [ ] **Step 6: Build app (rename + deploy are consistent, so this succeeds)**

Run: `cmake --build build --target App --config RelWithDebInfo`
Expected: EXIT 0. Verify deploy: `ls build/data/config/EngineSettings.toml` → exists. Delete the stale old copy: `rm -f build/data/config/engine.toml`.

- [ ] **Step 7: Commit**

```bash
git add resources/config/EngineSettings.toml src/engine/AetherCore.hpp src/engine/utils/EngineSettings.cpp src/engine/utils/EngineSettings.hpp src/app/CMakeLists.txt
git commit -m "Rename shipped/user settings files to EngineSettings.toml / UserSettings.toml"
```

### Task 1.3: Update package copy + verifier

**Files:**
- Modify: `src/app/CMakeLists.txt:268-269` (package copy — only exercised when packaging)
- Modify: `cmake/VerifyAppPackage.cmake:19`

- [ ] **Step 1: Update the package copy**

In `src/app/CMakeLists.txt` (around lines 268–269):
```cmake
        "${CMAKE_BINARY_DIR}/data/config/EngineSettings.toml"
        "${_aether_package_dir}/data/config/EngineSettings.toml"
```

- [ ] **Step 2: Update the package verifier**

`cmake/VerifyAppPackage.cmake:19`, change:
```cmake
    "data/config/EngineSettings.toml"
```

- [ ] **Step 3: Build app (packaging path reconfigures cleanly)**

Run: `cmake --build build --target App --config RelWithDebInfo`
Expected: EXIT 0.

- [ ] **Step 4: Commit**

```bash
git add src/app/CMakeLists.txt cmake/VerifyAppPackage.cmake
git commit -m "Update package copy and verifier for EngineSettings.toml"
```

**Phase 1 done when:** `EngineTests` all green, `App` builds, `build/data/config/EngineSettings.toml` deployed.

---

## Phase 2 — Relocate editor/debug state to LocalAppData

Editor state currently saves to `data/config/debug.toml` beside the exe (via `TomlConfig::SaveFile` → `ResolvePath`). It moves to `GetUserConfigDir()/EditorState.toml` and must NOT go through `TomlConfig::LoadFile`/`SaveFile` (those keep their `config://`/`ResolvePath` semantics for engine files).

**File Structure (Phase 2):**
- Modify: `src/engine/utils/TomlConfig.hpp` / `.cpp` — add absolute-path load/save entry points.
- Modify: `src/app/layers/DebugLayer.cpp:161-176` — resolve `EditorState.toml` under the user-config dir and use the new entry points.

### Task 2.1: Add path-based load/save to TomlConfig (test first)

**Files:**
- Test: `tests/utils/TomlConfigTests.cpp`
- Modify: `src/engine/utils/TomlConfig.hpp:16-18`, `src/engine/utils/TomlConfig.cpp`

- [ ] **Step 1: Write the failing test**

Append to `tests/utils/TomlConfigTests.cpp`:

```cpp
#include "io/FileUtil.hpp"      // add near the top with the other includes

TEST_CASE("TomlConfig: LoadFromPath/SaveToPath round-trips an absolute path") {
    const auto path = std::filesystem::temp_directory_path() / "aether_editorstate_test.toml";
    std::error_code ec; std::filesystem::remove(path, ec);

    TomlConfig out;
    out.Set("window.viewport", true);
    REQUIRE(out.SaveToPath(path, "Editor state"));

    TomlConfig in;
    REQUIRE(in.LoadFromPath(path));
    CHECK(in.GetBool("window.viewport", false) == true);

    std::filesystem::remove(path, ec);
}
```

- [ ] **Step 2: Run it — expect FAIL (no such member)**

Run: `cmake --build build --target EngineTests --config RelWithDebInfo`
Expected: FAIL to compile — `SaveToPath`/`LoadFromPath` not members of `TomlConfig`.

- [ ] **Step 3: Declare the new members**

`src/engine/utils/TomlConfig.hpp`, add under the existing `LoadFile`/`SaveFile` declarations:
```cpp
		[[nodiscard]] bool LoadFromPath(const std::filesystem::path& path);
		bool SaveToPath(const std::filesystem::path& path, std::string_view headerComment = {}) const;
```
Add `#include <filesystem>` to the header if not present.

- [ ] **Step 4: Implement them**

`src/engine/utils/TomlConfig.cpp`, add (they reuse the existing `Load`/`Save` string/stream primitives and `io::file_util`):
```cpp
	bool TomlConfig::LoadFromPath(const std::filesystem::path& path)
	{
		auto text = io::file_util::ReadText(path);
		if (!text)
		{
			return false;
		}
		Load(*text);
		return true;
	}

	bool TomlConfig::SaveToPath(const std::filesystem::path& path, std::string_view headerComment) const
	{
		std::ostringstream buffer;
		Save(buffer, headerComment);
		if (auto result = io::file_util::WriteText(path, buffer.str()); !result)
		{
			AE_WARN(LogCategory::Engine, "Failed to write config file: {} - {}", path.string(), result.error().message);
			return false;
		}
		return true;
	}
```
(`io/FileUtil.hpp`, `<sstream>`, `Logger.hpp` are already included in the .cpp.)

- [ ] **Step 5: Run tests — expect PASS**

Run: `cmake --build build --target EngineTests --config RelWithDebInfo && ./build/tests/RelWithDebInfo/EngineTests.exe --test-case="TomlConfig*"`
Expected: PASS (all TomlConfig cases).

- [ ] **Step 6: Commit**

```bash
git add tests/utils/TomlConfigTests.cpp src/engine/utils/TomlConfig.hpp src/engine/utils/TomlConfig.cpp
git commit -m "Add absolute-path load/save to TomlConfig"
```

### Task 2.2: Point DebugLayer at LocalAppData/EditorState.toml

**Files:**
- Modify: `src/app/layers/DebugLayer.cpp:161-176`

- [ ] **Step 1: Add the include**

At the top of `src/app/layers/DebugLayer.cpp`, ensure `#include "io/PlatformPaths.hpp"` is present.

- [ ] **Step 2: Add a path helper and switch load/save**

Replace `DebugLayer::LoadSettings` and `DebugLayer::SaveSettings` (lines ~161–176) with:
```cpp
	namespace
	{
		std::filesystem::path EditorStatePath()
		{
			return io::PlatformPaths::GetUserConfigDir() / "EditorState.toml";
		}
	} // namespace

	void DebugLayer::LoadSettings(LayerContext&)
	{
		const auto path = EditorStatePath();
		if (!path.empty() && m_debugConfig.LoadFromPath(path))
		{
			AE_INFO(LogCategory::App, "Editor state loaded from {}", path.string());
		}
		m_projects.LoadSettings(m_debugConfig);
	}

	void DebugLayer::SaveSettings(LayerContext&)
	{
		if (!m_debugConfig.IsDirty())
		{
			return;
		}
		const auto path = EditorStatePath();
		if (!path.empty() && m_debugConfig.SaveToPath(path, "AetherCore editor state"))
		{
			m_debugConfig.MarkClean();
			AE_INFO(LogCategory::App, "Editor state saved to {}", path.string());
		}
	}
```
Note: this replaces the old `SaveIfDirty("debug", ...)` path-resolution with an explicit LocalAppData path; the dirty-gate is preserved via `IsDirty()`/`MarkClean()`. If the anonymous `namespace { ... }` at the top of the file already exists (it does — `PanelVisibilityKey` lives there), add `EditorStatePath()` inside that existing block instead of opening a new one.

- [ ] **Step 3: Build the app**

Run: `cmake --build build --target App --config RelWithDebInfo`
Expected: EXIT 0.

- [ ] **Step 4: Manual verify (run the editor once, then check the file location)**

Launch the app (use the project's run path), open/close a panel to dirty state, exit. Then:
`ls "$LOCALAPPDATA/AetherCore/EditorState.toml"` → exists; `ls build/data/config/debug.toml` → not newly written.
Delete the stale legacy file if present: `rm -f build/data/config/debug.toml build-vs2022-msvc/data/config/debug.toml`.

- [ ] **Step 5: Commit**

```bash
git add src/app/layers/DebugLayer.cpp
git commit -m "Move editor state to LocalAppData/EditorState.toml"
```

**Phase 2 done when:** app builds, editor state writes to `LocalAppData/AetherCore/EditorState.toml`, nothing writes `debug.toml` in the build tree.

---

## Phase 3 — ProjectSettings.toml as the single project file + launcher file-open

`ProjectSettings.toml` at the project root replaces `.project/aether.project`, `settings/engine.toml`, and `.project/publish.toml`. The launcher opens the file; a folder is a project iff it contains `ProjectSettings.toml`. The project's `[app]`/`[window]`/`[graphics]` feed the settings cascade (Phase 1's project layer).

> Each task below begins by reading the named file — the UI/publisher internals (`ProjectLauncherWindow.cpp`, `ProjectPanel.cpp`, `EditorProjectPublisher.cpp`) were not fully transcribed into this plan; read them before editing so the edits match the surrounding code.

**File Structure (Phase 3):**
- Create: `resources/templates/ProjectSettings.toml` (new-project template).
- Delete: `resources/settings/engine.toml`.
- Modify: `src/app/debug/EditorProjectManager.cpp` — descriptor constants, read/write, discovery, `OpenProject`-by-file, project layer into cascade, template path.
- Modify: `src/app/editor/EditorProjectContext.hpp` — add `projectFile`, drop `settingsDir`.
- Modify: `src/app/debug/ProjectLauncherWindow.cpp` / `.hpp` — file picker action + button labels.
- Modify: `src/app/debug/ProjectPanel.cpp` — settings/publish paths → `ProjectSettings.toml`.
- Modify: `src/app/editor/EditorProjectPublisher.cpp` — read `[publish]` from `ProjectSettings.toml`; deploy `EngineSettings.toml`.
- Modify: `src/app/CMakeLists.txt:55,63` — `AETHER_DEFAULT_SETTINGS_DIR` → templates dir.
- Convert: `projects/TestingProject` — write root `ProjectSettings.toml`, delete `.project/` + `settings/`.

### Task 3.1: Create the template and the sample project's new file

**Files:**
- Create: `resources/templates/ProjectSettings.toml`
- Create: `projects/TestingProject/ProjectSettings.toml`
- Delete: `resources/settings/engine.toml`, `projects/TestingProject/.project/`, `projects/TestingProject/settings/`

- [ ] **Step 1: Write the new-project template**

Create `resources/templates/ProjectSettings.toml`:
```toml
# AetherCore project file. This IS the project — open it from the launcher.

[project]
version = 1
name = "AetherProject"

[paths]
assets  = "assets"
scenes  = "scenes"
prefabs = "assets/prefabs"
scripts = "scripts"

[app]
startupScene = "default"

[publish]
platformName = "Windows"
productName  = "AetherProject"
buildScripts = true
cleanOutput  = true
openAfter    = true
syncEditorPak = true
usePackageTemplate = true
verifyOutput = true
outputRoot   = ""
```

- [ ] **Step 2: Write the converted TestingProject file (merges its three old files)**

Create `projects/TestingProject/ProjectSettings.toml`:
```toml
# AetherCore project file.

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
targetFps = 0

[window]
width = 1920
height = 1080

[graphics]
vsync = false
fxaa = false
asyncCompute = true

[publish]
platformName = "Windows"
productName  = "TestingProject"
buildScripts = true
cleanOutput  = true
openAfter    = true
syncEditorPak = true
usePackageTemplate = true
verifyOutput = true
outputRoot   = "D:\\AetherCore\\projects\\TestingProject\\Builds"
```

- [ ] **Step 3: Delete the superseded files**

```bash
git rm resources/settings/engine.toml
git rm -r projects/TestingProject/.project
git rm projects/TestingProject/settings/engine.toml
```
(If `projects/TestingProject/settings/` has other files, keep them; only remove `engine.toml`. Verify with `ls projects/TestingProject/settings` first.)

- [ ] **Step 4: Commit the file moves (code still references old names — that's fixed next; commit anyway to keep the diff readable)**

```bash
git add resources/templates/ProjectSettings.toml projects/TestingProject/ProjectSettings.toml
git commit -m "Add ProjectSettings.toml template + convert TestingProject; drop legacy project files"
```

### Task 3.2: Read ProjectSettings.toml as the descriptor + feed the cascade

**Files:**
- Modify: `src/app/editor/EditorProjectContext.hpp`
- Modify: `src/app/debug/EditorProjectManager.cpp`

- [ ] **Step 1: Update the project context struct**

`src/app/editor/EditorProjectContext.hpp` — replace `settingsDir` with the project-file path:
```cpp
		std::filesystem::path root;
		std::filesystem::path projectFile;   // <root>/ProjectSettings.toml
		std::filesystem::path assetsDir;
		std::filesystem::path scenesDir;
		std::filesystem::path prefabsDir;
		std::filesystem::path scriptsDir;
		std::string name;
		bool loaded = false;
```

- [ ] **Step 2: Repoint the descriptor constants and path helpers**

In `src/app/debug/EditorProjectManager.cpp` anonymous namespace (lines ~33-108): the descriptor is now `ProjectSettings.toml` at the project root. Replace the `.project/aether.project` machinery:
```cpp
		constexpr int kMaxRecentProjects = 8;
		constexpr std::string_view kProjectFileName = "ProjectSettings.toml";

		std::filesystem::path ProjectFilePath(const std::filesystem::path& root)
		{
			return root / kProjectFileName;
		}

		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			return io::file_util::Exists(ProjectFilePath(root));
		}
```
Delete `ProjectDirectoryPath`, `DescriptorPath`, `LegacyDescriptorPath`, `ExistingDescriptorPath`. Update `ResolveProjectRoot` so that if `path.filename() == kProjectFileName` it strips to the parent (that is how a picked file resolves to the root):
```cpp
		std::filesystem::path ResolveProjectRoot(std::filesystem::path path)
		{
			path = NormalizePath(std::move(path));
			if (path.empty())
			{
				return {};
			}
			if (path.filename() == kProjectFileName)
			{
				path = path.parent_path();
			}
			return path;
		}
```

- [ ] **Step 3: Read the descriptor from ProjectSettings.toml**

In `ReadProjectDescriptor` (lines ~126-195), read from `ProjectFilePath(root)` instead of `ExistingDescriptorPath(root)`, set `project.projectFile`, and drop `settingsDir`. The `[project].name` and `[paths].*` keys are unchanged in meaning; keep the same `text::ParseToml` handler but remove the `paths.settings` branch and the `project.settingsDir` line. Set:
```cpp
			project.projectFile = ProjectFilePath(root);
```
Keep `assetsDir/scenesDir/prefabsDir/scriptsDir` resolution exactly as-is.

- [ ] **Step 4: Feed the project layer into the settings cascade on open**

Replace `ReadProjectStartupScene` usage in `RefreshServices` (lines ~518-534). Instead of pulling only `startupScene`, re-derive the whole cascade with the project file so `[window]`/`[graphics]`/`[app]` apply and `base` is correct:
```cpp
	void EditorProjectManager::RefreshServices()
	{
		if (m_services == nullptr)
		{
			return;
		}
		m_services->Register<EditorProjectContext>(m_currentProject);
		if (auto* settings = m_services->TryGet<aether::SettingsService>())
		{
			auto loaded = EngineSettingsIO::LoadLayered("EngineSettings.toml", m_currentProject.projectFile);
			settings->Values() = loaded.values;
			settings->Base()   = loaded.base;   // requires a Base() accessor — see Step 5
			settings->ApplyAll();               // push live-applicable values (resolution, fxaa, ...)
		}
	}
```
Delete the now-unused `ReadProjectStartupScene` helper.

- [ ] **Step 5: Expose `Base()` on SettingsService (if not already writable)**

Read `src/engine/utils/SettingsService.hpp`. If `m_base` has no non-const accessor, add:
```cpp
		[[nodiscard]] EngineSettings& Base() noexcept { return m_base; }
```
alongside the existing `Values()` accessor. (Needed so opening a project updates the delta-save baseline.)

- [ ] **Step 6: Update `OpenProject` error text + discovery**

`OpenProject` (lines ~492-516): update the error string from "No .project/aether.project found in that folder." to:
```cpp
			m_launcherState.error = "No ProjectSettings.toml found there.";
```
`ResolveProjectRoot` now accepts either a folder or the `ProjectSettings.toml` path, so `OpenProject` works for both.

- [ ] **Step 7: Update `WriteProjectDescriptor` to write ProjectSettings.toml**

`WriteProjectDescriptor` (lines ~273-313): stop creating `.project/`; write `ProjectFilePath(root)` by copying `resources/templates/ProjectSettings.toml` and substituting the name, OR emit the same content inline with `[project].name` set. Keep the folder-seeding loop but drop `"settings"` from the created-dirs list. Minimal inline version:
```cpp
			const std::string descriptor =
			    "# AetherCore project file.\n\n"
			    "[project]\nversion = 1\nname = \"" + EscapeTomlString(name) + "\"\n\n"
			    "[paths]\nassets = \"assets\"\nscenes = \"scenes\"\nprefabs = \"assets/prefabs\"\nscripts = \"scripts\"\n\n"
			    "[app]\nstartupScene = \"default\"\n\n"
			    "[publish]\nplatformName = \"Windows\"\nproductName = \"" + EscapeTomlString(name) + "\"\n";
			if (auto writeResult = io::file_util::WriteText(ProjectFilePath(root), descriptor); !writeResult)
			{
				error = "Could not write ProjectSettings.toml.";
				return false;
			}
```

- [ ] **Step 8: Add the include for EngineSettingsIO**

Ensure `#include "utils/EngineSettings.hpp"` is present in `EditorProjectManager.cpp`.

- [ ] **Step 9: Build the app**

Run: `cmake --build build --target App --config RelWithDebInfo`
Expected: EXIT 0. Fix any references to the removed `settingsDir` / `.project` helpers the compiler flags (see Tasks 3.3–3.4 for the known ones in ProjectPanel/Publisher).

- [ ] **Step 10: Commit**

```bash
git add src/app/editor/EditorProjectContext.hpp src/app/debug/EditorProjectManager.cpp src/engine/utils/SettingsService.hpp
git commit -m "Read ProjectSettings.toml as the project descriptor and feed the settings cascade"
```

### Task 3.3: Switch the launcher to a file picker

**Files:**
- Modify: `src/app/debug/EditorProjectManager.cpp` (`PickProjectFolder` → `PickProjectFile`, `DrawLauncher` action)
- Modify: `src/app/debug/ProjectLauncherWindow.cpp` / `.hpp`

- [ ] **Step 1: Read `ProjectLauncherWindow.hpp`/`.cpp` and `EditorProjectManager::DrawLauncher` (lines ~558-580)** to see the `ProjectLauncherWindowActions` struct and how `browseFolder` is invoked/labelled.

- [ ] **Step 2: Turn the Win32 picker into a file picker**

In `EditorProjectManager.cpp`, rename `PickProjectFolder` → `PickProjectFile` and drop `FOS_PICKFOLDERS`, add a `.toml` filter and title "Select ProjectSettings.toml":
```cpp
			dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
			dialog->SetTitle(L"Select ProjectSettings.toml");
			const COMDLG_FILTERSPEC filter[] = {{L"AetherCore project", L"ProjectSettings.toml"}, {L"TOML", L"*.toml"}};
			dialog->SetFileTypes(2, filter);
```
Point the `DrawLauncher` action (and the `ConfigureActions`/`m_actions.browseFolder` if present) at `PickProjectFile`. If the action field is named `browseFolder`, rename it to `browseProject` in the actions struct and both call sites.

- [ ] **Step 3: Update launcher button labels/help text**

In `ProjectLauncherWindow.cpp` (the browse buttons ~289, ~308 and any "select a folder" copy), change wording from folder to "Open project file…". Keep the `Create` flow pointing at a folder (new projects still choose a destination folder; the file is created inside it).

- [ ] **Step 4: Build the app**

Run: `cmake --build build --target App --config RelWithDebInfo`
Expected: EXIT 0.

- [ ] **Step 5: Commit**

```bash
git add src/app/debug/EditorProjectManager.cpp src/app/debug/ProjectLauncherWindow.cpp src/app/debug/ProjectLauncherWindow.hpp
git commit -m "Launcher opens ProjectSettings.toml via file picker"
```

### Task 3.4: Publish settings + paths read from ProjectSettings.toml

**Files:**
- Modify: `src/app/debug/ProjectPanel.cpp:40,45` (and publish read/write sites)
- Modify: `src/app/editor/EditorProjectPublisher.cpp:290,407`
- Modify: `src/app/CMakeLists.txt:55,63`

- [ ] **Step 1: Read `ProjectPanel.cpp` around the settings/publish helpers (lines ~40-55, and the publish read/write functions) and `EditorProjectPublisher.cpp` around lines 280-420** to see how `publish.toml` and the project `settings/engine.toml` are loaded/saved.

- [ ] **Step 2: Repoint ProjectPanel paths**

`ProjectPanel.cpp:40` — the settings-file accessor returned `project.settingsDir / "engine.toml"`; return `project.projectFile` (the root `ProjectSettings.toml`). `ProjectPanel.cpp:45` — the publish path returned `project.root / ".project" / "publish.toml"`; publish now lives in `project.projectFile`. Update the publish load/save to read/write the `[publish]` section of `ProjectSettings.toml`, using whole-file `TomlConfig` load → set `[publish].*` keys → save so other sections (`[project]`, `[paths]`, `[app]`, `[window]`, `[graphics]`) are preserved. Remove any `settings/` or `.project/` folder rows from `DrawFolderRow` usage (line ~545).

- [ ] **Step 3: Repoint the publisher**

`EditorProjectPublisher.cpp:290` and `:407` reference `data/config/engine.toml` for the deployed shipped settings — change to `EngineSettings.toml`. Where it reads project publish config, read the `[publish]` section from `<root>/ProjectSettings.toml`.

- [ ] **Step 4: Repoint the new-project template dir define**

`src/app/CMakeLists.txt:55` and `:63` set `AETHER_DEFAULT_SETTINGS_DIR="${CMAKE_SOURCE_DIR}/resources/settings"`. Since `SeedProjectTemplateFiles`/`WriteProjectDescriptor` no longer copy `resources/settings/engine.toml`, either (a) remove the now-unused define and its `CopyTemplateFile(AETHER_DEFAULT_SETTINGS_DIR, "engine.toml", ...)` call in `EditorProjectManager.cpp:249`, or (b) redefine it to `resources/templates` and copy `ProjectSettings.toml`. Prefer (a) if `WriteProjectDescriptor` already writes the file inline (Task 3.2 Step 7). Grep first: `grep -n AETHER_DEFAULT_SETTINGS_DIR src/app/debug/EditorProjectManager.cpp` and remove all uses if going with (a).

- [ ] **Step 5: Build app + run tests**

Run: `cmake --build build --target App --config RelWithDebInfo && cmake --build build --target EngineTests --config RelWithDebInfo && ./build/tests/RelWithDebInfo/EngineTests.exe`
Expected: app EXIT 0; tests all PASS.

- [ ] **Step 6: Manual verify the whole flow**

Launch the app → launcher → open `projects/TestingProject/ProjectSettings.toml` via the file picker. Confirm: project loads; window is 1920×1080 and vsync off (project layer applied); the Project panel shows correct folders; open the publish dialog, toggle a setting, publish/save → reopen `projects/TestingProject/ProjectSettings.toml` and confirm the `[publish]` change persisted and `[project]`/`[paths]`/`[app]`/`[window]`/`[graphics]` are intact.

- [ ] **Step 7: Commit**

```bash
git add src/app/debug/ProjectPanel.cpp src/app/editor/EditorProjectPublisher.cpp src/app/CMakeLists.txt src/app/debug/EditorProjectManager.cpp
git commit -m "Publish settings and paths read from ProjectSettings.toml"
```

**Phase 3 done when:** app builds, `EngineTests` green, the launcher opens `ProjectSettings.toml`, project-level window/graphics apply, and publish round-trips through the `[publish]` section without clobbering other sections.

---

## Final self-check (run after all phases)

- [ ] `git ls-files "*.toml" | grep -viE "^build"` shows: `resources/config/EngineSettings.toml`, `resources/templates/ProjectSettings.toml`, `projects/TestingProject/ProjectSettings.toml`, plus content files (`*.scene.toml`, `*.prefab.toml`, material `*.toml`). NO `engine.toml`, no `resources/settings/`, no `.project/`.
- [ ] `./build/tests/RelWithDebInfo/EngineTests.exe` — all green.
- [ ] Editor writes `LocalAppData/AetherCore/{UserSettings.toml, EditorState.toml}`; nothing writes `settings.toml`/`debug.toml`.
- [ ] Opening `ProjectSettings.toml` loads the project and applies its window/graphics/app overrides.
