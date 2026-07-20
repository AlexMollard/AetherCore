# Launcher Upgrade Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users remove/relocate projects from the launcher's recents (including missing ones) and make the Create/Open dialogs friendly (parent+name→subfolder with live preview and inline validation; folder-based Open with found/not-found feedback).

**Architecture:** Two pure path helpers (`SanitizeProjectFolderName`, `ComposeNewProjectRoot`) live in a new zero-platform-dependency TU so they are unit-testable and shared by the dialog preview and the layer. The launcher window (`ProjectLauncherWindow`) is the view; `LauncherLayer` owns the recents list and wires the callbacks. New `Actions` callbacks (`removeRecent`, `revealProjectFolder`, `relocateRecent`) plumb card interactions to the layer; `createProject`'s first argument becomes the *parent* dir and the wiring lambda composes the subfolder.

**Tech Stack:** C++20, Dear ImGui, `std::filesystem`, toml++ (recents persistence), doctest, the aethercore MCP + Launcher exe for runtime verification.

## Global Constraints

- Cross-platform: keep Windows + Linux building; platform code stays behind the existing `#ifdef _WIN32` split used in `ProjectCommon.cpp`.
- Build dir: `build/default` (MSVC, authoritative). `ninja-clang` has a pre-existing unrelated break in `ScreenshotService.cpp` — do not use it for verification.
- Prod compiler is MSVC; clang-tidy/clangd (clang-cl) errors are real — keep both clean.
- App/launcher/editor sources are `GLOB_RECURSE` (`src/app/CMakeLists.txt`); new `.cpp` files under `src/app/` are auto-picked-up. Test sources are globbed too; app-side TUs under test are listed explicitly in `tests/CMakeLists.txt`.
- `LauncherLayer::CreateProject(root, name, template)` and `OpenProject(root)` are ALSO called by the MCP control path (`launcher.new_project`/`launcher.open_project` → `QueueCreateProjectForControl`/`QueueOpenProjectForControl` → `LauncherLayer.cpp:330/334`). Do NOT change these methods' semantics — `root` stays the explicit target. Only the UI action lambda composes the subfolder.
- `Actions::createProject` keeps its TYPES (`std::filesystem::path, std::string_view, project::ProjectTemplate`); only the meaning of arg-1 changes (parent, not root), so the change is source-compatible.

---

## File Structure

- `src/app/project/ProjectPaths.hpp` (new) — declares the two pure helpers.
- `src/app/project/ProjectPaths.cpp` (new) — pure implementation (no platform headers).
- `tests/app/ProjectPathsTests.cpp` (new) — doctest unit tests.
- `tests/CMakeLists.txt` — add `ProjectPaths.cpp` to the EngineTests app-TU list.
- `src/app/project/ProjectCommon.hpp` / `.cpp` — add `OpenPathInFileManager(path)` platform helper.
- `src/app/launcher/LauncherLayer.hpp` / `.cpp` — `RemoveRecent`, `RelocateRecent`, `RevealProjectFolder`; wire the new actions; compose subfolder in the create lambda.
- `src/app/debug/ProjectLauncherWindow.hpp` — extend `Actions`.
- `src/app/debug/ProjectLauncherWindow.cpp` — card remove UX (hover ×, context menu); Create dialog redesign; Open dialog redesign.

---

### Task 1: Pure path helpers + unit tests

**Files:**
- Create: `src/app/project/ProjectPaths.hpp`, `src/app/project/ProjectPaths.cpp`
- Create: `tests/app/ProjectPathsTests.cpp`
- Modify: `tests/CMakeLists.txt` (add the app TU)

**Interfaces:**
- Produces:
  ```cpp
  namespace aether::app::project {
    // Sanitize a display name into a safe folder name: strip control + illegal
    // filename chars (\ / : * ? " < > |), trim ends, collapse internal whitespace
    // to single spaces. Returns "" if nothing usable remains.
    std::string SanitizeProjectFolderName(std::string_view name);
    // parent / SanitizeProjectFolderName(name), normalized. Empty parent or empty
    // sanitized name -> empty path.
    std::filesystem::path ComposeNewProjectRoot(const std::filesystem::path& parent, std::string_view name);
  }
  ```
  (Note: this refines the spec — the "AetherProject" fallback is NOT baked into `Sanitize`; empty-in → empty-out, so the dialog can disable Create and show validation.)

- [ ] **Step 1: Write the failing tests**

Create `tests/app/ProjectPathsTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "project/ProjectPaths.hpp"

using aether::app::project::ComposeNewProjectRoot;
using aether::app::project::SanitizeProjectFolderName;

TEST_CASE("SanitizeProjectFolderName keeps a clean name and trims/collapses spaces")
{
    CHECK(SanitizeProjectFolderName("My Game") == "My Game");
    CHECK(SanitizeProjectFolderName("  My   Game  ") == "My Game");
}

TEST_CASE("SanitizeProjectFolderName strips illegal filename and control characters")
{
    CHECK(SanitizeProjectFolderName("a/b:c*?\"<>|d") == "abcd");
    CHECK(SanitizeProjectFolderName(std::string_view("x\ty", 3)) == "x y");
}

TEST_CASE("SanitizeProjectFolderName returns empty when nothing usable remains")
{
    CHECK(SanitizeProjectFolderName("").empty());
    CHECK(SanitizeProjectFolderName("///:::").empty());
    CHECK(SanitizeProjectFolderName("   ").empty());
}

TEST_CASE("ComposeNewProjectRoot joins parent and sanitized name")
{
    const auto root = ComposeNewProjectRoot("C:/projects", "My Game");
    CHECK(root.generic_string() == "C:/projects/My Game");
}

TEST_CASE("ComposeNewProjectRoot is empty when parent or sanitized name is empty")
{
    CHECK(ComposeNewProjectRoot("", "X").empty());
    CHECK(ComposeNewProjectRoot("C:/projects", "///").empty());
}
```

- [ ] **Step 2: Add the app TU to EngineTests**

In `tests/CMakeLists.txt`, in the explicit app-TU list under `add_executable(EngineTests ...)` (after the `AsepriteSpriteImporter.cpp` line ~23), add:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/project/ProjectPaths.cpp"
```

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build/default --target EngineTests --config Debug`
Expected: compile/link FAIL (`ProjectPaths.hpp` not found / unresolved symbols) — the red state.

- [ ] **Step 4: Implement the helpers**

Create `src/app/project/ProjectPaths.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::app::project
{
	[[nodiscard]] std::string SanitizeProjectFolderName(std::string_view name);
	[[nodiscard]] std::filesystem::path ComposeNewProjectRoot(const std::filesystem::path& parent, std::string_view name);
} // namespace aether::app::project
```

Create `src/app/project/ProjectPaths.cpp`:

```cpp
#include "project/ProjectPaths.hpp"

#include <cctype>

namespace aether::app::project
{
	std::string SanitizeProjectFolderName(std::string_view name)
	{
		std::string out;
		out.reserve(name.size());
		bool pendingSpace = false;
		for (const char c: name)
		{
			const unsigned char uc = static_cast<unsigned char>(c);
			if (std::iscntrl(uc) != 0)
			{
				continue;
			}
			if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			{
				continue;
			}
			if (std::isspace(uc) != 0)
			{
				pendingSpace = !out.empty(); // drop leading whitespace
				continue;
			}
			if (pendingSpace)
			{
				out.push_back(' ');
				pendingSpace = false;
			}
			out.push_back(c);
		}
		return out; // pendingSpace never flushed at the end -> trailing space dropped
	}

	std::filesystem::path ComposeNewProjectRoot(const std::filesystem::path& parent, std::string_view name)
	{
		const std::string folder = SanitizeProjectFolderName(name);
		if (parent.empty() || folder.empty())
		{
			return {};
		}
		return (parent / folder).lexically_normal();
	}
} // namespace aether::app::project
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build/default --target EngineTests --config Debug && "D:/AetherCore/build/default/tests/Debug/EngineTests.exe" --test-case="*ProjectFolderName*,*ComposeNewProjectRoot*"`
Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/app/project/ProjectPaths.hpp src/app/project/ProjectPaths.cpp tests/app/ProjectPathsTests.cpp tests/CMakeLists.txt
git commit -m "feat(launcher): pure project-folder path helpers + tests"
```

---

### Task 2: Reveal-in-file-manager platform helper

**Files:**
- Modify: `src/app/project/ProjectCommon.hpp` (declare), `src/app/project/ProjectCommon.cpp` (impl, near `PickProjectFolder` ~line 385)

**Interfaces:**
- Produces: `void OpenPathInFileManager(const std::filesystem::path& path);` — opens a folder (or a file's containing folder) in the OS file manager; no-op on empty/nonexistent.

- [ ] **Step 1: Declare the helper**

In `src/app/project/ProjectCommon.hpp`, near `PickProjectFolder()` (line ~64), add:

```cpp
	// Open a folder (or a file's parent folder) in the OS file manager. No-op if
	// the path is empty or does not exist.
	void OpenPathInFileManager(const std::filesystem::path& path);
```

- [ ] **Step 2: Implement behind the platform split**

In `src/app/project/ProjectCommon.cpp`, inside `#ifdef _WIN32` region (near `PickProjectFolder`), and a POSIX branch, add:

```cpp
#ifdef _WIN32
	void OpenPathInFileManager(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (path.empty() || !std::filesystem::exists(path, ec))
		{
			return;
		}
		const std::filesystem::path folder = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
		ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
#else
	void OpenPathInFileManager(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (path.empty() || !std::filesystem::exists(path, ec))
		{
			return;
		}
		const std::filesystem::path folder = std::filesystem::is_directory(path, ec) ? path : path.parent_path();
		const std::string cmd = "xdg-open \"" + folder.string() + "\" >/dev/null 2>&1 &";
		std::system(cmd.c_str());
	}
#endif
```

Add `#include <cstdlib>` (for `std::system`) and `<system_error>` (already included) at the top of `ProjectCommon.cpp` if not present. Ensure this lives inside `namespace aether::app::project`.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles (helper unused so far is fine; it is called in Task 3).

- [ ] **Step 4: Commit**

```bash
git add src/app/project/ProjectCommon.hpp src/app/project/ProjectCommon.cpp
git commit -m "feat(launcher): OpenPathInFileManager platform helper"
```

---

### Task 3: Layer — remove/relocate/reveal methods + action wiring + Actions struct

**Files:**
- Modify: `src/app/launcher/LauncherLayer.hpp` (declare methods)
- Modify: `src/app/launcher/LauncherLayer.cpp` (impl + wire actions ~440-481)
- Modify: `src/app/debug/ProjectLauncherWindow.hpp` (extend `Actions`)

**Interfaces:**
- Consumes: `project::OpenPathInFileManager`, `project::PickProjectFolder`, `project::ResolveProjectRoot`, `project::HasProjectDescriptor`, `project::NormalizePath`, existing `RememberRecent`, `PersistSettings`.
- Produces on `LauncherLayer`:
  ```cpp
  void RemoveRecent(const std::filesystem::path& root);
  void RelocateRecent(const std::filesystem::path& oldRoot);
  void RevealProjectFolder(const std::filesystem::path& root);
  ```
- Produces on `ProjectLauncherWindowActions`:
  ```cpp
  std::function<void(std::filesystem::path)> removeRecent;
  std::function<void(std::filesystem::path)> revealProjectFolder;
  std::function<void(std::filesystem::path)> relocateRecent;
  ```

- [ ] **Step 1: Extend the Actions struct**

In `src/app/debug/ProjectLauncherWindow.hpp`, inside `struct ProjectLauncherWindowActions` (after `saveSettings`, line ~60), add:

```cpp
		std::function<void(std::filesystem::path)> removeRecent;
		std::function<void(std::filesystem::path)> revealProjectFolder;
		std::function<void(std::filesystem::path)> relocateRecent;
```

- [ ] **Step 2: Declare the layer methods**

In `src/app/launcher/LauncherLayer.hpp`, near `RememberRecent` (find its declaration), add:

```cpp
		void RemoveRecent(const std::filesystem::path& root);
		void RelocateRecent(const std::filesystem::path& oldRoot);
		void RevealProjectFolder(const std::filesystem::path& root);
```

- [ ] **Step 3: Implement the methods**

In `src/app/launcher/LauncherLayer.cpp`, after `RememberRecent` (ends ~line 586), add:

```cpp
	void LauncherLayer::RemoveRecent(const std::filesystem::path& root)
	{
		const std::filesystem::path resolved = project::NormalizePath(project::ResolveProjectRoot(root));
		const std::size_t before = m_recentProjects.size();
		std::erase_if(m_recentProjects, [&](const EditorProjectContext& p) { return project::NormalizePath(p.root) == resolved; });
		if (m_recentProjects.size() != before)
		{
			PersistSettings();
		}
	}

	void LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot)
	{
		m_windowState.error.clear();
		const auto picked = project::PickProjectFolder();
		if (!picked.has_value())
		{
			return; // user cancelled
		}
		const std::filesystem::path resolved = project::ResolveProjectRoot(*picked);
		if (!project::HasProjectDescriptor(resolved))
		{
			m_windowState.error = "That folder has no ProjectSettings.toml.";
			return;
		}
		RemoveRecent(oldRoot);
		RememberRecent(resolved); // moves to front + persists
	}

	void LauncherLayer::RevealProjectFolder(const std::filesystem::path& root)
	{
		project::OpenPathInFileManager(project::ResolveProjectRoot(root));
	}
```

- [ ] **Step 4: Wire the new actions + compose the subfolder on create**

In `src/app/launcher/LauncherLayer.cpp` `OnImGui` action wiring (~458-478), change the `createProject` lambda to treat arg-1 as the parent and compose the subfolder, and add the three new actions:

```cpp
		actions.createProject = [this](const std::filesystem::path& parentDir, std::string_view name, project::ProjectTemplate projectTemplate)
		{
			const std::filesystem::path root = project::ComposeNewProjectRoot(parentDir, name);
			if (root.empty())
			{
				m_windowState.error = "Choose a location and a valid project name.";
				return;
			}
			CreateProject(root, name, projectTemplate);
		};
		actions.removeRecent = [this](const std::filesystem::path& root) { RemoveRecent(root); };
		actions.revealProjectFolder = [this](const std::filesystem::path& root) { RevealProjectFolder(root); };
		actions.relocateRecent = [this](const std::filesystem::path& root) { RelocateRecent(root); };
```

Add `#include "project/ProjectPaths.hpp"` to `LauncherLayer.cpp` includes.

- [ ] **Step 5: Build**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles. (The Create dialog still passes its single path field as arg-1; the friendly parent field lands in Task 5. Removal/reveal/relocate are wired but unused by the view until Task 4.)

- [ ] **Step 6: Commit**

```bash
git add src/app/launcher/LauncherLayer.hpp src/app/launcher/LauncherLayer.cpp src/app/debug/ProjectLauncherWindow.hpp
git commit -m "feat(launcher): layer support for remove/relocate/reveal + subfolder compose"
```

---

### Task 4: Card remove UX — hover × + right-click context menu

**Files:**
- Modify: `src/app/debug/ProjectLauncherWindow.cpp` — `DrawProjectCard` signature + body (~89-170), and its call site in `PaintRecentsGrid` (~471)

**Interfaces:**
- Consumes: `actions.removeRecent`, `actions.revealProjectFolder`, `actions.relocateRecent`, `actions.openProject`.
- Produces: `DrawProjectCard(...)` returns an enum action for the frame so the caller dispatches; signature gains the actions + missing flag (already has `missing`).

- [ ] **Step 1: Add a card-result type and extend the signature**

At the top of the anonymous namespace in `ProjectLauncherWindow.cpp`, add:

```cpp
	enum class CardAction { None, Open, Remove, Reveal, Relocate };
```

Change `DrawProjectCard` to return `CardAction` and take the project root context it already has. Replace the final `return pressed;` logic (see Step 2). Update the declaration line (~89) to:

```cpp
	CardAction DrawProjectCard(const EditorProjectContext& project, std::uint64_t previewTextureId, const std::string& modifiedLabel, bool selected, bool missing, const ImVec2& cardSize, const Px& dp)
```

- [ ] **Step 2: Hover × hit region + context menu, returning the action**

In `DrawProjectCard`, after the full-card `InvisibleButton` (line ~93) capture its rect, and before `return`, add the × button and context menu. Replace the tail of the function (from the `else if (hovered)` OPEN hint block through `return pressed;`) with:

```cpp
		CardAction action = CardAction::None;

		// Right-click context menu bound to this card item.
		if (ImGui::BeginPopupContextItem("##cardMenu"))
		{
			if (!missing && ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open"))
			{
				action = CardAction::Open;
			}
			if (!missing && ImGui::MenuItem(ICON_FA_FOLDER "  Reveal folder"))
			{
				action = CardAction::Reveal;
			}
			if (missing && ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Locate moved project..."))
			{
				action = CardAction::Relocate;
			}
			ImGui::Separator();
			if (ImGui::MenuItem(ICON_FA_TRASH "  Remove from list"))
			{
				action = CardAction::Remove;
			}
			ImGui::EndPopup();
		}

		// Hover close (x) button in the top-right corner.
		const float xSize = dp(22.0f);
		const ImVec2 xMin(end.x - xSize - dp(6.0f), start.y + dp(6.0f));
		const ImVec2 xMax(xMin.x + xSize, xMin.y + xSize);
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool xHovered = hovered && mouse.x >= xMin.x && mouse.x <= xMax.x && mouse.y >= xMin.y && mouse.y <= xMax.y;
		if (hovered || xHovered)
		{
			drawList->AddRectFilled(xMin, xMax, ToU32(WithAlpha(xHovered ? kError : kBg, xHovered ? 0.9f : 0.6f)), dp(3.0f));
			const ImVec2 gx = MeasureSized(dp(13.0f), ICON_FA_XMARK);
			TextSized(drawList, dp(13.0f), ImVec2((xMin.x + xMax.x - gx.x) * 0.5f, (xMin.y + xMax.y - gx.y) * 0.5f), xHovered ? kOnAccent : kMuted, ICON_FA_XMARK);
		}
		if (xHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			action = CardAction::Remove;
		}

		// The full-card press opens, but only when the x was not the target and the
		// project is present.
		if (action == CardAction::None && pressed && !xHovered && !missing)
		{
			action = CardAction::Open;
		}

		ImGui::PopID();
		return action;
```

(Keep the existing OPEN-hint / MISSING-tag drawing above this block; only the `else if (hovered)` OPEN-hint may remain for visual affordance. Remove the old `ImGui::PopID(); return pressed;` at the very end since this block now owns them.)

Confirm `ICON_FA_FOLDER`, `ICON_FA_MAGNIFYING_GLASS`, `ICON_FA_TRASH`, `ICON_FA_XMARK` exist in `debug/Icons.hpp`; if a glyph is absent, substitute the nearest present icon (grep `Icons.hpp`).

- [ ] **Step 3: Dispatch the action at the call site**

In `PaintRecentsGrid` (~471), replace the `if (DrawProjectCard(...) && actions.openProject) { actions.openProject(project.root); }` with:

```cpp
					switch (DrawProjectCard(project, preview, edited, selected, missing, ImVec2(cardWidth, cardHeight), dp))
					{
						case CardAction::Open:
							if (actions.openProject) actions.openProject(project.root);
							break;
						case CardAction::Remove:
							if (actions.removeRecent) actions.removeRecent(project.root);
							break;
						case CardAction::Reveal:
							if (actions.revealProjectFolder) actions.revealProjectFolder(project.root);
							break;
						case CardAction::Relocate:
							if (actions.relocateRecent) actions.relocateRecent(project.root);
							break;
						case CardAction::None:
							break;
					}
```

Note: removing an item mutates `model.recentProjects` (via the layer) — since the layer rebuilds the model span next frame and the removal happens through a callback, do not continue iterating the current `recentProjects` after a structural change within the same frame. The callback defers the mutation safely because `removeRecent` edits `m_recentProjects` while the view iterates `model.recentProjects` (the same span). To avoid iterating a mutated span, capture the action and apply the open/remove AFTER the loop: collect `pendingAction`/`pendingRoot` during the loop and dispatch once after `ImGui::EndChild()`. Implement that: declare `CardAction pending = CardAction::None; std::filesystem::path pendingRoot;` before the loop, set them instead of calling the action inline, and run the switch once after the grid loop.

- [ ] **Step 4: Build**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add src/app/debug/ProjectLauncherWindow.cpp
git commit -m "feat(launcher): remove/reveal/relocate recents via hover x + context menu"
```

---

### Task 5: Create dialog — parent + name + live preview + validation

**Files:**
- Modify: `src/app/debug/ProjectLauncherWindow.hpp` — add a parent-path buffer to state
- Modify: `src/app/debug/ProjectLauncherWindow.cpp` — Create branch of `DrawProjectDialog` (~308-356, 366-395)

**Interfaces:**
- Consumes: `project::SanitizeProjectFolderName`, `project::ComposeNewProjectRoot`, `actions.browseFolder`, `actions.createProject(parentDir, name, template)`.

- [ ] **Step 1: Add a parent buffer to state**

In `ProjectLauncherWindow.hpp` `ProjectLauncherWindowState`, the existing `newPath` buffer becomes the PARENT location (rename for clarity is optional; keep `newPath` to minimize churn — it now holds the parent dir). No new field required.

- [ ] **Step 2: Rewrite the Create branch**

Add `#include "project/ProjectPaths.hpp"` to `ProjectLauncherWindow.cpp`. Replace the `else` (create) branch body inside `DrawProjectDialog` (the block after `ImGui::TextUnformatted("Create a new AetherCore project");`) with:

```cpp
			ImGui::TextUnformatted("Create a new AetherCore project");
			ImGui::TextDisabled("Pick a template, name it, and choose where it lives.");
			ImGui::Spacing();

			const project::ProjectTemplateInfo* selectedTemplate = &project::kProjectTemplates.front();
			for (const project::ProjectTemplateInfo& info: project::kProjectTemplates)
			{
				if (info.value == state.newTemplate)
				{
					selectedTemplate = &info;
					break;
				}
			}
			ImGui::TextUnformatted("Template");
			ImGui::SetNextItemWidth(width);
			if (ImGui::BeginCombo("##newProjectTemplate", selectedTemplate->name.data()))
			{
				for (const project::ProjectTemplateInfo& info: project::kProjectTemplates)
				{
					const bool sel = info.value == state.newTemplate;
					if (ImGui::Selectable(info.name.data(), sel))
					{
						state.newTemplate = info.value;
						selectedTemplate = &info;
					}
					if (sel)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
				ImGui::EndCombo();
			}
			ImGui::TextDisabled("%s", selectedTemplate->description.data());
			ImGui::Spacing();

			ImGui::TextUnformatted("Name");
			ImGui::SetNextItemWidth(width);
			submit = ImGui::InputTextWithHint("##newProjectName", "My Game", state.newName.data(), state.newName.size(), ImGuiInputTextFlags_EnterReturnsTrue);

			ImGui::TextUnformatted("Location");
			ImGui::SetNextItemWidth(width - browseSize - controlGap);
			submit = ImGui::InputTextWithHint("##newProjectPath", "Parent folder for the new project...", state.newPath.data(), state.newPath.size(), ImGuiInputTextFlags_EnterReturnsTrue) || submit;
			ImGui::SameLine(0.0f, controlGap);
			if (OutlineIconButton(ICON_FA_FOLDER_OPEN, "##browseNew", ImVec2(browseSize, browseSize)) && actions.browseFolder)
			{
				if (const auto folder = actions.browseFolder())
				{
					CopyToBuffer(state.newPath, *folder);
				}
			}

			// Live preview + inline validation.
			const std::filesystem::path composed = project::ComposeNewProjectRoot(std::filesystem::path(state.newPath.data()), state.newName.data());
			ImGui::Spacing();
			if (!composed.empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
				ImGui::TextWrapped("Creates: %s", composed.lexically_normal().string().c_str());
				ImGui::PopStyleColor();
				std::error_code existsEc;
				if (std::filesystem::exists(composed, existsEc) && !std::filesystem::is_empty(composed, existsEc))
				{
					ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
					ImGui::TextWrapped("A non-empty folder already exists here - files may be overwritten.");
					ImGui::PopStyleColor();
				}
			}
			else if (state.newName[0] != '\0' && project::SanitizeProjectFolderName(state.newName.data()).empty())
			{
				ImGui::PushStyleColor(ImGuiCol_Text, kError);
				ImGui::TextWrapped("That name has no usable characters.");
				ImGui::PopStyleColor();
			}
```

If `kWarn` does not exist in the chrome palette, use `kAccentHi` (grep `EditorChrome.hpp` for the available colours).

- [ ] **Step 3: Update the create submit + incomplete check**

In `DrawProjectDialog`, change the `incomplete` computation (~367) and the create submit branch (~389-392):

```cpp
		const std::filesystem::path createRoot = opening ? std::filesystem::path{} : project::ComposeNewProjectRoot(std::filesystem::path(state.newPath.data()), state.newName.data());
		const bool incomplete = opening ? state.openPath[0] == '\0' : createRoot.empty();
```

```cpp
			else if (!opening && actions.createProject)
			{
				actions.createProject(std::filesystem::path(state.newPath.data()), state.newName.data(), state.newTemplate);
			}
```

- [ ] **Step 4: Build**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles.

- [ ] **Step 5: Commit**

```bash
git add src/app/debug/ProjectLauncherWindow.cpp src/app/debug/ProjectLauncherWindow.hpp
git commit -m "feat(launcher): friendly Create dialog with live path preview + validation"
```

---

### Task 6: Open dialog — folder-based with inline found/not-found status

**Files:**
- Modify: `src/app/debug/ProjectLauncherWindow.cpp` — Open branch of `DrawProjectDialog` (~292-307), and its wiring uses `actions.browseFolder`

**Interfaces:**
- Consumes: `project::ResolveProjectRoot`, `project::HasProjectDescriptor`, `actions.browseFolder`.

- [ ] **Step 1: Rewrite the Open branch**

Replace the `if (opening) { ... }` body:

```cpp
		if (opening)
		{
			ImGui::TextUnformatted("Open an existing AetherCore project");
			ImGui::TextDisabled("Choose the project folder (or its ProjectSettings.toml).");
			ImGui::Spacing();
			ImGui::SetNextItemWidth(width - browseSize - controlGap);
			submit = ImGui::InputTextWithHint("##openProjectPath", "Project folder...", state.openPath.data(), state.openPath.size(), ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine(0.0f, controlGap);
			if (OutlineIconButton(ICON_FA_FOLDER_OPEN, "##browseOpen", ImVec2(browseSize, browseSize)) && actions.browseFolder)
			{
				if (const auto folder = actions.browseFolder())
				{
					CopyToBuffer(state.openPath, *folder);
				}
			}
			ImGui::Spacing();
			if (state.openPath[0] != '\0')
			{
				const bool found = project::HasProjectDescriptor(project::ResolveProjectRoot(std::filesystem::path(state.openPath.data())));
				ImGui::PushStyleColor(ImGuiCol_Text, found ? kAccentHi : kError);
				ImGui::TextWrapped(found ? ICON_FA_CHECK "  Project found" : ICON_FA_XMARK "  No ProjectSettings.toml here");
				ImGui::PopStyleColor();
			}
		}
```

- [ ] **Step 2: Gate Open on a valid, present project**

In the `incomplete` computation, extend the opening case so Open is disabled unless a descriptor is present:

```cpp
		const bool openReady = !opening ? false : (state.openPath[0] != '\0' && project::HasProjectDescriptor(project::ResolveProjectRoot(std::filesystem::path(state.openPath.data()))));
		const std::filesystem::path createRoot = opening ? std::filesystem::path{} : project::ComposeNewProjectRoot(std::filesystem::path(state.newPath.data()), state.newName.data());
		const bool incomplete = opening ? !openReady : createRoot.empty();
```

(Replace the `incomplete` line from Task 5 Step 3 with this combined version.)

Confirm `ICON_FA_CHECK` exists in `debug/Icons.hpp`; substitute if absent.

- [ ] **Step 3: Build**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles.

- [ ] **Step 4: Commit**

```bash
git add src/app/debug/ProjectLauncherWindow.cpp
git commit -m "feat(launcher): folder-based Open dialog with inline project detection"
```

---

### Task 7: Runtime verification

**Files:** none (verification only).

**Preconditions:** `cmake --build build/default --target Launcher --config Debug` green; `EngineTests` green.

- [ ] **Step 1: Seed a missing recent**

Locate the launcher state file (`LauncherStatePath()` — under `%LOCALAPPDATA%/AetherCore/`). With the launcher NOT running, add (or confirm) a recent-projects entry pointing at a non-existent path (e.g. `D:/AetherCore/projects/_DeletedProject`), so a card renders as MISSING. (Alternatively open then delete a throwaway project folder.)

- [ ] **Step 2: Launch the Launcher with the control port**

Run (background), from the exe's own dir so assets resolve:
```bash
cd "D:/AetherCore/build/default/src/app/Debug" && AETHER_CONTROL_PORT=8790 ./Launcher.exe > /tmp/launcher.log 2>&1 &
```
Confirm via `mcp__aethercore__launcher_info` (after loading the tool) that it reports a recent-project count.

- [ ] **Step 3: Screenshot + inspect the hub**

Use `mcp__aethercore__screenshot`. Verify: the missing card shows the MISSING tag; hovering shows the × ; right-click shows the context menu with *Remove from list* and *Locate moved project…*. (Interaction that needs mouse input is confirmed visually; the removal logic is unit-covered by the layer path and the pure helpers.)

- [ ] **Step 4: Screenshot the Create dialog**

Trigger the Create dialog (New Project button). Screenshot and verify: separate Name and Location fields, a live `Creates: <parent>/<name>` line, and the Create button disabled until both are filled.

- [ ] **Step 5: Screenshot the Open dialog**

Trigger Open. Screenshot and verify the inline `Project found` / `No ProjectSettings.toml here` status updates with the path, and Open is disabled when not found.

- [ ] **Step 6: Close the launcher + record results**

Stop the Launcher process. Note pass/fail per check. If any fails, return to the owning task.

---

## Self-Review

**Spec coverage:**
- Remove from recents (present/missing) → Task 3 (`RemoveRecent`) + Task 4 (× / menu). ✓
- Relocate moved project → Task 3 (`RelocateRecent`) + Task 4 (menu). ✓
- Reveal folder → Task 2 (`OpenPathInFileManager`) + Task 3 (`RevealProjectFolder`) + Task 4 (menu). ✓
- Create parent+name→subfolder + preview + validation → Task 1 (helpers) + Task 3 (wiring) + Task 5 (dialog). ✓
- Open folder-based + inline status → Task 6. ✓
- Shared pure helpers → Task 1. ✓
- MCP create/open semantics preserved → Global Constraints + Task 3 (compose in lambda, not in `CreateProject`). ✓
- Cross-platform reveal → Task 2 (`#ifdef _WIN32` / `xdg-open`). ✓
- Testing (unit + runtime) → Task 1 (unit), Task 7 (runtime). ✓

**Placeholder scan:** No TBD/TODO. UI tasks are build- + runtime-verified (ImGui has no unit harness) — stated explicitly, not hand-waved. Icon-glyph existence has an explicit "grep Icons.hpp and substitute" instruction rather than assuming.

**Type consistency:** `CardAction` enum used consistently in Task 4 (return + dispatch). `ComposeNewProjectRoot(parent, name)` / `SanitizeProjectFolderName(name)` signatures identical across Tasks 1/3/5/6. `Actions::createProject(path, string_view, ProjectTemplate)` types unchanged; only arg-1 meaning shifts (documented). New `Actions` callbacks (`removeRecent`/`revealProjectFolder`/`relocateRecent`) declared in Task 3, consumed in Task 4. `OpenPathInFileManager` declared Task 2, used Task 3.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-17-launcher-upgrade.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks.
2. **Inline Execution** — execute here with checkpoints.

Which approach?
