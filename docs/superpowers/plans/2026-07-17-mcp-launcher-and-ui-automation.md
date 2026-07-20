# MCP Launcher Methods + ImGui UI Automation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the launcher's recents operations as MCP methods, and add an ImGui automation layer (query on-screen widgets + simulate mouse/keyboard) so the editor/launcher UI can be driven and verified headlessly.

**Architecture:** A custom automation layer built on ImGui's official `IMGUI_ENABLE_TEST_ENGINE` item hooks (per-frame widget registry) plus a pure synthetic-input state machine injected into ImGui's IO event queue each frame before `NewFrame`. Handlers run on the existing main-thread `DrainCommands` pump; actions are async (enqueue + return, effect visible by the next tool call). MCP methods are shared across the editor and launcher control endpoints.

**Tech Stack:** C++20, Dear ImGui 1.92.8-docking, GLFW/Vulkan backends, enet ControlServer, nlohmann/json, doctest, the aethercore MCP + Launcher/Editor exes for runtime verification.

## Global Constraints

- Cross-platform: keep Windows + Linux building; no platform-specific automation code (ImGui IO injection is portable).
- Build dir: `build/default` (MSVC, authoritative). `ninja-clang` has a pre-existing unrelated break — do not use it for verification.
- MCP handlers run on the **main loop thread** via `ControlServer::DrainCommands` (`ControlServerLayer.cpp:54`). Do NOT add a worker thread. UI-automation actions are async (multi-frame); a handler must never block waiting for an action to finish.
- `LauncherLayer::CreateProject`/`OpenProject` MCP semantics are unchanged (see prior launcher plan).
- App sources are `GLOB_RECURSE` (`src/app/CMakeLists.txt`) but split per target: `Editor` = all app sources minus `launcher/`; `Launcher` = app sources minus `debug/`, `editor/`, `scripting/`, `systems/`, `scene/` (+ explicitly re-adds `ProjectLauncherWindow.cpp` and `ControlServer.cpp`); `GameRuntime` = app sources minus `imgui/`, `debug/`, `editor/`, `launcher/`, `project/`. **Only Editor, Launcher, and EngineTests link imgui** (`CMakeLists.txt:75,303`; tests link it too). `GameRuntime` links only `Engine` — no imgui.
- **Placement matters:** put UI-automation TUs under `src/app/imgui/` — that dir is compiled into BOTH Editor and Launcher (Launcher excludes `editor/`, so `editor/`-placed files would be missing from the Launcher). `imgui/` is excluded from `GameRuntime`, so these TUs never reach the imgui-free runtime.
- **Linkage rule:** enabling `IMGUI_ENABLE_TEST_ENGINE` makes `imgui.cpp` reference `ImGuiTestEngineHook_*` symbols; every imgui-linking target must provide them. `UiAutomation.cpp` (under `imgui/`) implements them and is auto-compiled into Editor + Launcher; it MUST also be added explicitly to `EngineTests`. `GameRuntime` doesn't link imgui, so it needs neither the symbols nor these TUs.
- Exact hook signatures come from the vendored `imgui_internal.h:4294-4298` — match them verbatim.
- `tools/mcp/manifest.json` reflects the live method table; regenerate + commit it whenever methods change.

---

## File Structure

- `src/app/launcher/LauncherLayer.hpp` / `.cpp` — `RelocateRecent(old,new,error)` overload; 3 new control methods.
- `cmake/Dependencies.cmake` — `IMGUI_ENABLE_TEST_ENGINE` PUBLIC define on the imgui target.
- `src/app/imgui/UiInputScript.hpp` / `.cpp` (new) — pure synthetic-input state machine (no ImGui globals).
- `src/app/imgui/UiAutomation.hpp` / `.cpp` (new) — singleton: item registry + the 4 hook symbols + input applier.
- `src/app/imgui/ImguiSubsystem.cpp` — set `TestEngineHookItems`; per-frame registry swap + input apply.
- `src/app/imgui/UiAutomationMethods.hpp` / `.cpp` (new) — `AppendUiAutomationMethods` + local prop helpers. Placed under `imgui/` (not `editor/`) so the Launcher build — which excludes `editor/` — still compiles it.
- `src/app/editor/ControlMethods.cpp` — call `AppendUiAutomationMethods`.
- `src/app/launcher/LauncherLayer.cpp` — call `AppendUiAutomationMethods`.
- `tests/app/UiInputScriptTests.cpp`, `tests/app/UiAutomationTests.cpp` (new) — unit tests.
- `tests/CMakeLists.txt` — add `UiInputScript.cpp`, `UiAutomation.cpp` to the EngineTests app-TU list.

---

## Phase 1 — Launcher control methods

### Task 1: Headless relocate core

**Files:**
- Modify: `src/app/launcher/LauncherLayer.hpp` (declare overload)
- Modify: `src/app/launcher/LauncherLayer.cpp` (`RelocateRecent` split ~589)

**Interfaces:**
- Produces: `bool LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot, const std::filesystem::path& newRoot, std::string& error);` — validates `newRoot` has a descriptor, then removes old + remembers new. Returns true on success; sets `error` + returns false otherwise.

- [ ] **Step 1: Declare the overload**

In `LauncherLayer.hpp`, next to the existing `void RelocateRecent(const std::filesystem::path& oldRoot);`, add:

```cpp
		bool RelocateRecent(const std::filesystem::path& oldRoot, const std::filesystem::path& newRoot, std::string& error);
```

- [ ] **Step 2: Implement the core and delegate the UI version to it**

In `LauncherLayer.cpp`, replace the existing `RelocateRecent(oldRoot)` body with a picker that delegates, and add the core:

```cpp
	void LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot)
	{
		m_windowState.error.clear();
		const auto picked = project::PickProjectFolder();
		if (!picked.has_value())
		{
			return; // user cancelled
		}
		std::string error;
		if (!RelocateRecent(oldRoot, project::ResolveProjectRoot(*picked), error))
		{
			m_windowState.error = error;
		}
	}

	bool LauncherLayer::RelocateRecent(const std::filesystem::path& oldRoot, const std::filesystem::path& newRoot, std::string& error)
	{
		const std::filesystem::path resolved = project::ResolveProjectRoot(newRoot);
		if (!project::HasProjectDescriptor(resolved))
		{
			error = "That folder has no ProjectSettings.toml.";
			return false;
		}
		RemoveRecent(oldRoot);
		RememberRecent(resolved); // moves to front + persists
		return true;
	}
```

- [ ] **Step 3: Build**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles.

- [ ] **Step 4: Commit**

```bash
git add src/app/launcher/LauncherLayer.hpp src/app/launcher/LauncherLayer.cpp
git commit -m "feat(launcher): headless RelocateRecent(old,new,error) core"
```

---

### Task 2: Launcher MCP methods + manifest

**Files:**
- Modify: `src/app/launcher/LauncherLayer.cpp` (`BuildLauncherControlMethods` ~102-195)
- Modify: `tools/mcp/manifest.json` (regenerated)

**Interfaces:**
- Consumes: `LauncherLayer::RemoveRecent`, `RevealProjectFolder`, `RelocateRecent(old,new,error)`, `RecentProjects()`.

- [ ] **Step 1: Add the three methods**

In `BuildLauncherControlMethods`, before `return methods;`, add (reuse the local `Obj`/`StrProp` helpers already in that TU, and the `RecentProjects()` -> json array pattern from `list_projects`):

```cpp
		methods.push_back({"launcher.remove_recent",
		        "remove_recent",
		        "Remove a project from the Launcher's recent list (works whether the project still exists or is missing).",
		        true,
		        Obj({{"root", StrProp()}}, {"root"}),
		        [&launcher](const json& params, editor::MethodContext&) -> json
		        {
			        const std::filesystem::path root = params.value("root", std::string{});
			        const std::size_t before = launcher.RecentProjects().size();
			        launcher.RemoveRecent(root);
			        json projects = json::array();
			        for (const EditorProjectContext& p: launcher.RecentProjects())
			        {
				        projects.push_back(json{{"name", p.name}, {"root", p.root.string()}});
			        }
			        return json{{"removed", launcher.RecentProjects().size() != before}, {"projects", std::move(projects)}};
		        }});

		methods.push_back({"launcher.reveal_folder",
		        "reveal_folder",
		        "Open a recent project's folder in the OS file manager.",
		        false,
		        Obj({{"root", StrProp()}}, {"root"}),
		        [&launcher](const json& params, editor::MethodContext&) -> json
		        {
			        const std::filesystem::path root = params.value("root", std::string{});
			        launcher.RevealProjectFolder(root);
			        return json{{"status", "opened"}, {"root", root.string()}};
		        }});

		methods.push_back({"launcher.relocate_recent",
		        "relocate_recent",
		        "Re-point a moved project in the recent list to a new folder (headless: no folder picker).",
		        true,
		        Obj({{"old_root", StrProp()}, {"new_root", StrProp()}}, {"old_root", "new_root"}),
		        [&launcher](const json& params, editor::MethodContext&) -> json
		        {
			        const std::filesystem::path oldRoot = params.value("old_root", std::string{});
			        const std::filesystem::path newRoot = params.value("new_root", std::string{});
			        std::string error;
			        if (!launcher.RelocateRecent(oldRoot, newRoot, error))
			        {
				        return json{{"error", error}};
			        }
			        json projects = json::array();
			        for (const EditorProjectContext& p: launcher.RecentProjects())
			        {
				        projects.push_back(json{{"name", p.name}, {"root", p.root.string()}});
			        }
			        return json{{"projects", std::move(projects)}};
		        }});
```

- [ ] **Step 2: Build**

Run: `cmake --build build/default --target Launcher --config Debug`
Expected: compiles.

- [ ] **Step 3: Regenerate the manifest**

The manifest is written by the app from its live method table. Launch the launcher briefly to regenerate, then confirm the new tools appear:

```bash
cd "D:/AetherCore/build/default/src/app/Debug" && AETHER_CONTROL_PORT=8787 ./Launcher.exe > /tmp/launcher.log 2>&1 &
sleep 6
```
Then (from repo root) `git diff --stat tools/mcp/manifest.json` should show additions for `remove_recent`, `reveal_folder`, `relocate_recent`. Stop the launcher (`powershell -Command "Get-Process Launcher | Stop-Process -Force"`). If the manifest is generated elsewhere (grep for where `manifest.json` is written — search `manifest` in `src/`/`tools/`), run that generator instead.

- [ ] **Step 4: Runtime check via MCP**

With the launcher running on 8787, call (MCP): `list_projects`, then `remove_recent {root: <a recent root>}`, then `list_projects` again — confirm the entry is gone and `removed=true`.

- [ ] **Step 5: Commit**

```bash
git add src/app/launcher/LauncherLayer.cpp tools/mcp/manifest.json
git commit -m "feat(mcp): launcher remove_recent / reveal_folder / relocate_recent"
```

---

## Phase 2 — UI automation core (registry + input)

### Task 3: Pure synthetic-input state machine + tests

**Files:**
- Create: `src/app/imgui/UiInputScript.hpp`, `src/app/imgui/UiInputScript.cpp`
- Create: `tests/app/UiInputScriptTests.cpp`
- Modify: `tests/CMakeLists.txt` (add `UiInputScript.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  namespace aether::app {
    enum class SynKind : std::uint8_t { MousePos, MouseButton, Key, Char };
    struct SynEvent { SynKind kind; float x=0, y=0; int button=0; bool down=false; int key=0; unsigned int ch=0; };
    class UiInputScript {
    public:
      void QueueClick(float x, float y, int button, bool doubleClick);
      void QueueHover(float x, float y);        // held until changed/cleared
      void QueueKey(int imguiKey);
      void QueueText(std::string utf8);         // Ctrl+A, Delete, then chars
      void ClearHover();
      std::vector<SynEvent> Step();             // events for one frame; advances state
      [[nodiscard]] bool Busy() const;          // true while a queued action is mid-play
    };
  }
  ```
  `Step()` always re-emits the held hover position (if any) first, then the current action's events. `button`: 0=left,1=right. `key` uses `ImGuiKey` values (passed as int to keep this TU ImGui-free).

- [ ] **Step 1: Write the failing tests**

Create `tests/app/UiInputScriptTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "imgui/UiInputScript.hpp"

using namespace aether::app;

static int CountKind(const std::vector<SynEvent>& evs, SynKind k)
{
    int n = 0;
    for (const auto& e: evs) if (e.kind == k) ++n;
    return n;
}

TEST_CASE("UiInputScript click plays pos, pos+down, up over three frames")
{
    UiInputScript s;
    s.QueueClick(100.0f, 50.0f, 0, false);
    CHECK(s.Busy());

    const auto f1 = s.Step(); // hover/pos established
    CHECK(CountKind(f1, SynKind::MousePos) == 1);
    CHECK(CountKind(f1, SynKind::MouseButton) == 0);

    const auto f2 = s.Step(); // button down
    REQUIRE(CountKind(f2, SynKind::MouseButton) == 1);
    CHECK(f2.back().down == true);
    CHECK(f2.back().button == 0);

    const auto f3 = s.Step(); // button up
    REQUIRE(CountKind(f3, SynKind::MouseButton) == 1);
    CHECK(f3.back().down == false);

    CHECK_FALSE(s.Busy());
    CHECK(s.Step().empty()); // idle
}

TEST_CASE("UiInputScript hover holds position across frames until cleared")
{
    UiInputScript s;
    s.QueueHover(10.0f, 20.0f);
    for (int i = 0; i < 3; ++i)
    {
        const auto f = s.Step();
        REQUIRE(CountKind(f, SynKind::MousePos) == 1);
        CHECK(f.front().x == doctest::Approx(10.0f));
    }
    s.ClearHover();
    CHECK(s.Step().empty());
}

TEST_CASE("UiInputScript double click plays two button cycles")
{
    UiInputScript s;
    s.QueueClick(0.0f, 0.0f, 0, true);
    int downs = 0, ups = 0;
    while (s.Busy())
    {
        for (const auto& e: s.Step())
        {
            if (e.kind == SynKind::MouseButton) (e.down ? downs : ups)++;
        }
    }
    CHECK(downs == 2);
    CHECK(ups == 2);
}

TEST_CASE("UiInputScript key plays a down then up")
{
    UiInputScript s;
    s.QueueKey(525 /* arbitrary ImGuiKey int */);
    const auto f1 = s.Step();
    REQUIRE(CountKind(f1, SynKind::Key) == 1);
    CHECK(f1.back().down == true);
    const auto f2 = s.Step();
    REQUIRE(CountKind(f2, SynKind::Key) == 1);
    CHECK(f2.back().down == false);
    CHECK_FALSE(s.Busy());
}

TEST_CASE("UiInputScript text clears then emits a char per code point")
{
    UiInputScript s;
    s.QueueText("Hi");
    bool sawChars = false;
    while (s.Busy())
    {
        for (const auto& e: s.Step())
        {
            if (e.kind == SynKind::Char) sawChars = true;
        }
    }
    CHECK(sawChars);
}
```

- [ ] **Step 2: Add the TU to EngineTests + run to verify it fails**

In `tests/CMakeLists.txt`, add to the app-TU list:
```cmake
    "${CMAKE_SOURCE_DIR}/src/app/imgui/UiInputScript.cpp"
```
Run: `cmake --build build/default --target EngineTests --config Debug`
Expected: FAIL (header/symbols missing).

- [ ] **Step 3: Implement `UiInputScript`**

Create `src/app/imgui/UiInputScript.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aether::app
{
	enum class SynKind : std::uint8_t { MousePos, MouseButton, Key, Char };

	struct SynEvent
	{
		SynKind kind;
		float x = 0.0f;
		float y = 0.0f;
		int button = 0; // 0 = left, 1 = right
		bool down = false;
		int key = 0;          // ImGuiKey value, kept as int to stay ImGui-free
		unsigned int ch = 0;  // UTF-32 code point
	};

	// Frame-stepped synthetic input. One queued action plays over several Step()
	// calls (frames); a held hover position is re-emitted every Step until cleared.
	class UiInputScript
	{
	public:
		void QueueClick(float x, float y, int button, bool doubleClick);
		void QueueHover(float x, float y);
		void QueueKey(int imguiKey);
		void QueueText(std::string utf8);
		void ClearHover();

		[[nodiscard]] std::vector<SynEvent> Step();
		[[nodiscard]] bool Busy() const;

	private:
		struct Action; // internal step list
		std::vector<SynEvent> m_steps; // flattened per-frame frames
		std::vector<std::size_t> m_frameBoundaries;
		std::size_t m_cursor = 0;      // index into m_frameBoundaries
		std::optional<std::pair<float, float>> m_hover;

		void PushFrame(std::vector<SynEvent> frame);
	};
} // namespace aether::app
```

Create `src/app/imgui/UiInputScript.cpp`:

```cpp
#include "imgui/UiInputScript.hpp"

namespace aether::app
{
	namespace
	{
		// Decode UTF-8 into code points (minimal, valid-input assumption).
		std::vector<unsigned int> DecodeUtf8(const std::string& s)
		{
			std::vector<unsigned int> out;
			for (std::size_t i = 0; i < s.size();)
			{
				const unsigned char c = static_cast<unsigned char>(s[i]);
				unsigned int cp = 0;
				int extra = 0;
				if (c < 0x80) { cp = c; extra = 0; }
				else if ((c >> 5) == 0x6) { cp = c & 0x1F; extra = 1; }
				else if ((c >> 4) == 0xE) { cp = c & 0x0F; extra = 2; }
				else if ((c >> 3) == 0x1E) { cp = c & 0x07; extra = 3; }
				else { ++i; continue; }
				++i;
				for (int k = 0; k < extra && i < s.size(); ++k, ++i)
				{
					cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
				}
				out.push_back(cp);
			}
			return out;
		}

		// ImGuiKey values used here without including imgui (kept in sync by name).
		constexpr int kKeyLeftCtrl = 641; // ImGuiKey_LeftCtrl (1.92); remap in the applier if needed
		constexpr int kKeyA = 546;        // ImGuiKey_A
		constexpr int kKeyDelete = 523;   // ImGuiKey_Delete
	} // namespace

	void UiInputScript::PushFrame(std::vector<SynEvent> frame)
	{
		m_frameBoundaries.push_back(m_steps.size());
		for (auto& e: frame)
		{
			m_steps.push_back(e);
		}
		m_frameBoundaries.push_back(m_steps.size());
	}

	void UiInputScript::QueueHover(float x, float y)
	{
		m_hover = std::make_pair(x, y);
	}

	void UiInputScript::ClearHover()
	{
		m_hover.reset();
	}

	void UiInputScript::QueueClick(float x, float y, int button, bool doubleClick)
	{
		m_hover = std::make_pair(x, y);
		const int cycles = doubleClick ? 2 : 1;
		PushFrame({SynEvent{SynKind::MousePos, x, y}});
		for (int c = 0; c < cycles; ++c)
		{
			PushFrame({SynEvent{SynKind::MousePos, x, y}, SynEvent{SynKind::MouseButton, x, y, button, true}});
			PushFrame({SynEvent{SynKind::MouseButton, x, y, button, false}});
		}
	}

	void UiInputScript::QueueKey(int imguiKey)
	{
		PushFrame({SynEvent{SynKind::Key, 0, 0, 0, true, imguiKey}});
		PushFrame({SynEvent{SynKind::Key, 0, 0, 0, false, imguiKey}});
	}

	void UiInputScript::QueueText(std::string utf8)
	{
		// Select-all + delete, then type.
		PushFrame({SynEvent{SynKind::Key, 0, 0, 0, true, kKeyLeftCtrl}, SynEvent{SynKind::Key, 0, 0, 0, true, kKeyA}});
		PushFrame({SynEvent{SynKind::Key, 0, 0, 0, false, kKeyA}, SynEvent{SynKind::Key, 0, 0, 0, false, kKeyLeftCtrl}});
		PushFrame({SynEvent{SynKind::Key, 0, 0, 0, true, kKeyDelete}});
		PushFrame({SynEvent{SynKind::Key, 0, 0, 0, false, kKeyDelete}});
		std::vector<SynEvent> chars;
		for (const unsigned int cp: DecodeUtf8(utf8))
		{
			chars.push_back(SynEvent{SynKind::Char, 0, 0, 0, false, 0, cp});
		}
		PushFrame(std::move(chars));
	}

	std::vector<SynEvent> UiInputScript::Step()
	{
		std::vector<SynEvent> out;
		if (m_hover.has_value())
		{
			out.push_back(SynEvent{SynKind::MousePos, m_hover->first, m_hover->second});
		}
		if (Busy())
		{
			const std::size_t begin = m_frameBoundaries[m_cursor];
			const std::size_t end = m_frameBoundaries[m_cursor + 1];
			for (std::size_t i = begin; i < end; ++i)
			{
				out.push_back(m_steps[i]);
			}
			m_cursor += 2;
			if (m_cursor >= m_frameBoundaries.size())
			{
				m_steps.clear();
				m_frameBoundaries.clear();
				m_cursor = 0;
			}
		}
		// When idle and no hover held, Step returns empty (tests rely on this).
		if (!m_hover.has_value() && out.size() == 0)
		{
			return {};
		}
		return out;
	}

	bool UiInputScript::Busy() const
	{
		return m_cursor < m_frameBoundaries.size();
	}
} // namespace aether::app
```

(The `kKey*` int constants are placeholders synced by name to `ImGuiKey`; the applier in Task 5 maps SynEvent.key ints straight to `ImGuiKey`, so define these constants to the real `ImGuiKey` enum values by including imgui in the .cpp instead of hardcoding — do that in Step 3 by `#include <imgui.h>` and using `ImGuiKey_LeftCtrl` etc. Replace the three `constexpr int kKey* = ...;` lines with `const int kKeyLeftCtrl = ImGuiKey_LeftCtrl; const int kKeyA = ImGuiKey_A; const int kKeyDelete = ImGuiKey_Delete;`. imgui.h is header-only-safe to include without a context.)

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build/default --target EngineTests --config Debug && "D:/AetherCore/build/default/tests/Debug/EngineTests.exe" --test-case="*UiInputScript*"`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add src/app/imgui/UiInputScript.hpp src/app/imgui/UiInputScript.cpp tests/app/UiInputScriptTests.cpp tests/CMakeLists.txt
git commit -m "feat(ui-automation): pure synthetic-input state machine + tests"
```

---

### Task 4: Item registry + ImGui hooks + subsystem wiring

**Files:**
- Modify: `cmake/Dependencies.cmake` (imgui define, ~172)
- Create: `src/app/imgui/UiAutomation.hpp`, `src/app/imgui/UiAutomation.cpp`
- Modify: `src/app/imgui/ImguiSubsystem.cpp` (context flag + per-frame swap/apply)
- Create: `tests/app/UiAutomationTests.cpp`
- Modify: `tests/CMakeLists.txt` (add `UiAutomation.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  namespace aether::app {
    struct UiItem { unsigned int id; std::string label; std::string window; float x, y, w, h; };
    class UiAutomation {
    public:
      static UiAutomation& Get();
      // main thread, called by ImguiSubsystem each frame:
      void BeginFrameSwap();                       // publish building -> snapshot, clear building
      void ApplyInput(class ImGuiIO& io);          // drain UiInputScript into io events
      // MCP-thread-safe reads/writes (same thread as ImguiSubsystem):
      const std::vector<UiItem>& Snapshot() const;
      std::optional<UiItem> FindItem(const std::string& window, const std::string& label, std::string& err) const;
      UiInputScript& Input();
      // hook sinks (called from the extern symbols):
      void RecordItemAdd(unsigned int id, float x, float y, float w, float h, const char* window);
      void RecordItemInfo(unsigned int id, const char* label);
      const char* DebugLabel(unsigned int id) const;
    };
  }
  ```
  `FindItem`: exact label match within `window` (if non-empty), else first label match across windows; ambiguous or missing -> sets `err`, returns nullopt. Center point = `{x + w/2, y + h/2}`.

- [ ] **Step 1: Enable the define**

In `cmake/Dependencies.cmake`, in the `if(imgui_ADDED)` block, add a PUBLIC define so imgui TUs emit the hook calls and consumers see the macro:

```cmake
    target_compile_definitions(imgui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
```

- [ ] **Step 2: Write the registry-lookup failing test**

Create `tests/app/UiAutomationTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "imgui/UiAutomation.hpp"

using namespace aether::app;

TEST_CASE("UiAutomation FindItem resolves by window+label and computes centre")
{
    UiAutomation& a = UiAutomation::Get();
    a.RecordItemAdd(1u, 10.0f, 20.0f, 100.0f, 40.0f, "Lighting");
    a.RecordItemInfo(1u, "Light gizmos");
    a.RecordItemAdd(2u, 0.0f, 0.0f, 50.0f, 50.0f, "Other");
    a.RecordItemInfo(2u, "Light gizmos");
    a.BeginFrameSwap(); // publish

    std::string err;
    const auto hit = a.FindItem("Lighting", "Light gizmos", err);
    REQUIRE(hit.has_value());
    CHECK(hit->x + hit->w / 2.0f == doctest::Approx(60.0f));
    CHECK(hit->y + hit->h / 2.0f == doctest::Approx(40.0f));

    const auto ambiguous = a.FindItem("", "Light gizmos", err);
    CHECK_FALSE(ambiguous.has_value());
    CHECK_FALSE(err.empty());

    const auto missing = a.FindItem("Lighting", "Nope", err);
    CHECK_FALSE(missing.has_value());
}
```

Add `"${CMAKE_SOURCE_DIR}/src/app/imgui/UiAutomation.cpp"` to the EngineTests app-TU list in `tests/CMakeLists.txt`. Run the build — expect FAIL (missing header).

- [ ] **Step 3: Implement `UiAutomation`**

Create `src/app/imgui/UiAutomation.hpp` (declares the struct/class above; include `<optional>`, `<string>`, `<vector>`, `<unordered_map>`, and forward-declare `ImGuiIO`). Create `src/app/imgui/UiAutomation.cpp`:

- Store a `building` vector<UiItem> + `unordered_map<id,index>` for ItemInfo label fill; `snapshot` vector published by `BeginFrameSwap` (`snapshot = std::move(building); building.clear(); index.clear();`).
- `RecordItemAdd(id,x,y,w,h,window)`: push `UiItem{id,"",window,x,y,w,h}`, map id->index.
- `RecordItemInfo(id,label)`: if id in building, set its label.
- `DebugLabel(id)`: look up label in snapshot (or building); return c_str or "".
- `FindItem`: iterate snapshot; collect matches; apply window filter + ambiguity/missing rules.
- `ApplyInput(io)`: `for (auto& e : Input().Step())` translate to `io.AddMousePosEvent(e.x,e.y)`, `io.AddMouseButtonEvent(e.button, e.down)`, `io.AddKeyEvent(static_cast<ImGuiKey>(e.key), e.down)`, `io.AddInputCharacter(e.ch)`.
- `Get()`: function-local static singleton.

Then implement the four extern hook symbols (match `imgui_internal.h:4294-4298` exactly), forwarding into the singleton. Include `<imgui.h>` and `<imgui_internal.h>` (the latter for `ImRect`/`ImGuiLastItemData`/`ImGuiItemStatusFlags`):

```cpp
#include "imgui/UiAutomation.hpp"

#include <imgui.h>
#include <imgui_internal.h>

// Hook symbols required by imgui.cpp when IMGUI_ENABLE_TEST_ENGINE is defined.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*)
{
	const char* window = (ctx != nullptr && ctx->CurrentWindow != nullptr) ? ctx->CurrentWindow->Name : "";
	aether::app::UiAutomation::Get().RecordItemAdd(static_cast<unsigned int>(id), bb.Min.x, bb.Min.y, bb.Max.x - bb.Min.x, bb.Max.y - bb.Min.y, window);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags)
{
	aether::app::UiAutomation::Get().RecordItemInfo(static_cast<unsigned int>(id), label != nullptr ? label : "");
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id)
{
	return aether::app::UiAutomation::Get().DebugLabel(static_cast<unsigned int>(id));
}
```

`ApplyInput` and `Snapshot`/`FindItem` live in the same TU (they include imgui for `ImGuiIO`/`ImGuiKey`). Guard `hovered`/`active` in the query (Task 5) via `ImGui::GetCurrentContext()->HoveredId`/`ActiveId`.

- [ ] **Step 4: Wire into ImguiSubsystem**

In `ImguiSubsystem.cpp` `Init` (after `ImGui::CreateContext();` line 96): set the hook gate:
```cpp
		if (ImGuiContext* g = ImGui::GetCurrentContext())
		{
			g->TestEngineHookItems = true;
		}
```
In `BeginFrame`, right before `ImGui::NewFrame();` (line 199) and after `ImGui_ImplGlfw_NewFrame();`:
```cpp
		UiAutomation::Get().BeginFrameSwap();
		UiAutomation::Get().ApplyInput(io);
```
(`io` is already in scope; add `#include "imgui/UiAutomation.hpp"`.)

- [ ] **Step 5: Build + run tests**

Run: `cmake --build build/default --target EngineTests --config Debug && "D:/AetherCore/build/default/tests/Debug/EngineTests.exe" --test-case="*UiAutomation*,*UiInputScript*"`
Then build every imgui-linking target to confirm the hook symbols resolve, and build `GameRuntime` to confirm imgui stays out of the runtime:
`cmake --build build/default --target Launcher --config Debug && cmake --build build/default --target Editor --config Debug && cmake --build build/default --target GameRuntime --config Debug`
Expected: tests PASS; Editor/Launcher/EngineTests link the hook symbols; GameRuntime still builds without imgui (it never compiles `imgui/` sources).

- [ ] **Step 6: Commit**

```bash
git add cmake/Dependencies.cmake src/app/imgui/UiAutomation.hpp src/app/imgui/UiAutomation.cpp src/app/imgui/ImguiSubsystem.cpp tests/app/UiAutomationTests.cpp tests/CMakeLists.txt
git commit -m "feat(ui-automation): item registry via imgui test-engine hooks + input applier"
```

---

## Phase 3 — MCP surface

### Task 5: `ui.*` control methods, shared by editor + launcher

**Files:**
- Create: `src/app/imgui/UiAutomationMethods.hpp`, `src/app/imgui/UiAutomationMethods.cpp` (under `imgui/` so both Editor and Launcher compile it; Launcher excludes `editor/`)
- Modify: `src/app/editor/ControlMethods.cpp` (call the appender in `BuildControlMethods`)
- Modify: `src/app/launcher/LauncherLayer.cpp` (call the appender in `BuildLauncherControlMethods`)
- Modify: `tools/mcp/manifest.json` (regenerated)

**Interfaces:**
- Produces: `void aether::editor::AppendUiAutomationMethods(std::vector<ControlMethod>& methods);`
- Consumes: `UiAutomation::Get()` (query, FindItem, Input()).

- [ ] **Step 1: Implement the appender**

`UiAutomationMethods.hpp` declares `void AppendUiAutomationMethods(std::vector<ControlMethod>& methods);` in `namespace aether::editor` (include `editor/ControlMethods.hpp` for the `ControlMethod` type; a header include is fine even though the Launcher excludes `editor/` *sources*). `UiAutomationMethods.cpp` defines local `Obj/StrProp/NumProp/BoolProp` helpers (copy the small ones from `ControlMethods.cpp`) and pushes five methods. Key handler logic:

- `ui_query`: read `UiAutomation::Get().Snapshot()`, filter by optional `window`/`label` substring, and for each item compute `hovered = (ImGui::GetCurrentContext()->HoveredId == id)`, `active = (ActiveId == id)`. Return `{items:[{window,label,x,y,w,h,hovered,active}]}`.
- `ui_click`: if `x`/`y` present use them; else `FindItem(window,label,err)` -> centre, on `err` return `{error, candidates?}`. Then `UiAutomation::Get().Input().QueueClick(cx, cy, button=="right"?1:0, double)`. Return `{status:"queued", target:{x,y}}`.
- `ui_hover`: resolve target like click, `Input().QueueHover(cx,cy)`.
- `ui_input_text`: resolve target, `QueueClick` (to focus) then `QueueText(text)`.
- `ui_key`: map a small name table (`"enter"->ImGuiKey_Enter`, `escape`, `tab`, `backspace`, `delete`, `left`,`right`,`up`,`down`) to int; `Input().QueueKey(key)`; unknown name -> `{error}`.

Mark `ui.query` non-mutating, the rest mutating.

- [ ] **Step 2: Wire into both endpoints**

In `ControlMethods.cpp` `BuildControlMethods`, after `Append2DAuthoringMethods(methods);` add `AppendUiAutomationMethods(methods);` (include the header). In `LauncherLayer.cpp` `BuildLauncherControlMethods`, before `return methods;` add `editor::AppendUiAutomationMethods(methods);` (include the header).

- [ ] **Step 3: Build**

Run: `cmake --build build/default --target Launcher --config Debug && cmake --build build/default --target Editor --config Debug`
Expected: both link.

- [ ] **Step 4: Regenerate + commit manifest**

Launch the launcher briefly (as in Task 2 Step 3) to regenerate `tools/mcp/manifest.json`; confirm `ui_query`/`ui_click`/`ui_hover`/`ui_input_text`/`ui_key` appear.

- [ ] **Step 5: Commit**

```bash
git add src/app/editor/UiAutomationMethods.hpp src/app/editor/UiAutomationMethods.cpp src/app/editor/ControlMethods.cpp src/app/launcher/LauncherLayer.cpp tools/mcp/manifest.json
git commit -m "feat(mcp): ui.query/click/hover/input_text/key automation methods"
```

---

## Phase 4 — Verification

### Task 6: Runtime end-to-end via MCP

**Files:** none (verification only). Restart the MCP server if new tools don't appear (per the 2D-authoring MCP note: a server restart is needed to see newly added tools).

- [ ] **Step 1: Launch launcher on 8787 with a seeded missing recent** (as in the launcher-upgrade Task 7): back up `LauncherState.toml`, point one entry at a non-existent path, launch from `build/default/src/app/Debug`.

- [ ] **Step 2: Query the UI** — `ui_query {window: "AetherCoreProjectLauncher"}` (or no filter); confirm items report sane rects and labels.

- [ ] **Step 3: Drive removal via the × ** — `ui_hover` the missing card's centre (reveals the ×), `ui_query` to read the card rect, `ui_click {x, y}` at the ×'s top-right corner, then `list_projects` to confirm the entry is gone. Cross-check `remove_recent` independently on another entry.

- [ ] **Step 4: Drive the context menu** — `ui_click {button:"right", x, y}` on a card, `ui_query` the popup, `ui_click {window:"##cardMenu"?, label:"Remove from list"}` (use the label reported by ui_query), confirm removal.

- [ ] **Step 5: Drive the Create dialog** — `ui_click {label:"New Project"}`, `ui_query` to read Name/Location fields and the disabled Create button, `ui_input_text` a name + location, `viewport.screenshot`, and confirm the live `Creates:` preview via screenshot.

- [ ] **Step 6: Editor smoke** — open a project (or launch the editor on 8787), `ui_query` a docked panel, confirm items from that window are reported.

- [ ] **Step 7: Restore state + record results** — restore `LauncherState.toml` from backup, close the launcher/editor, note pass/fail per check. If any fails, return to the owning task.

---

## Self-Review

**Spec coverage:**
- Launcher remove/reveal/relocate methods -> Tasks 1-2. ✓
- Headless relocate core -> Task 1. ✓
- Manifest regeneration -> Tasks 2, 5. ✓
- Item registry via IMGUI_ENABLE_TEST_ENGINE hooks -> Task 4. ✓
- Synthetic input state machine (click/hover/key/text, async multi-frame) -> Task 3 (pure) + Task 4 (applier) + Task 5 (queueing via MCP). ✓
- ui.query/click/hover/input_text/key, shared editor+launcher -> Task 5. ✓
- Main-thread model / async actions -> Global Constraints + Task 3/4 design. ✓
- Linkage rule (hook symbols in every imgui target) -> Global Constraints + Task 4 Step 5 (build all three). ✓
- Testing (unit input + registry; runtime end-to-end) -> Tasks 3, 4, 6. ✓

**Placeholder scan:** No TBD/TODO. The `kKey*` constants have an explicit "replace hardcoded ints with `ImGuiKey_*` by including imgui.h" instruction (Task 3 Step 3 note) — not left as a guess. Manifest-generation path has an explicit "grep for where manifest.json is written if the launch-to-regenerate step doesn't update it" fallback.

**Type consistency:** `SynEvent`/`SynKind`/`UiInputScript` identical across Task 3 (def), Task 4 (`ApplyInput` consumes), Task 5 (queue). `UiItem`/`UiAutomation::Get()`/`FindItem(window,label,err)` consistent Task 4 -> Task 5. `AppendUiAutomationMethods(std::vector<ControlMethod>&)` in `aether::editor`, consumed by both endpoints (both use `editor::ControlMethod`). Hook signatures copied verbatim from `imgui_internal.h:4294-4298`.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-17-mcp-launcher-and-ui-automation.md`. Two execution options:

1. **Subagent-Driven (recommended)** — fresh subagent per task, review between tasks.
2. **Inline Execution** — execute here with checkpoints.

Which approach?
