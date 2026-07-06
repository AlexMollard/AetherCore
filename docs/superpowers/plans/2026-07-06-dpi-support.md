# DPI Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-monitor DPI for the AetherCore editor — crisp fonts, per-monitor window scaling (incl. torn-out viewports), a user `uiScale` knob, and a DPI-aware "Match Panel" scene-resolution mode.

**Architecture:** Enable ImGui 1.92's native DPI model (`io.ConfigDpiScaleFonts` + `io.ConfigDpiScaleViewports`); stop overriding `io.DisplaySize` so `imgui_impl_glfw` owns the DPI-aware display metrics; add a `graphics.uiScale` setting mapped to `style.FontScaleMain`; add a `MatchPanel` scene-viewport mode that renders at the panel's physical pixel size.

**Tech Stack:** C++20, Dear ImGui v1.92.8-docking, GLFW 3.4, Vulkan, doctest, CMake (MSVC `build-vs2022-msvc`).

**Spec:** `docs/superpowers/specs/2026-07-06-dpi-support-design.md`
**Branch:** `claude/imgui-multiviewport` (DPI builds on the multi-viewport work).

---

## Conventions

- **Build:** `cmake --build build-vs2022-msvc --config Debug --target App EngineTests` (MSVC is the working build; `build-ninja-clang` is pre-existing broken — do not use it).
- **Tests:** `build-vs2022-msvc/tests/Debug/EngineTests.exe`.
- **Run (from build root):** `cd build-vs2022-msvc && ./src/app/Debug/App.exe`.
- New engine files auto-register (GLOB_RECURSE); a new **test** file must be added to `tests/CMakeLists.txt` (not needed here — we append to an existing test file).
- Commit after every task.

---

## Task 1: `graphics.uiScale` setting (TDD)

**Files:**
- Modify: `src/engine/utils/EngineSettings.hpp`
- Modify: `src/engine/utils/EngineSettings.cpp` (`Sanitize`)
- Test: `tests/utils/EngineSettingsTests.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/utils/EngineSettingsTests.cpp`:

```cpp
TEST_CASE("uiScale defaults to 1 and Sanitize clamps to [0.5, 3.0]") {
    EngineSettings s;
    CHECK(s.graphics.uiScale == doctest::Approx(1.0f));

    EngineSettingsIO::Apply("[graphics]\nuiScale = 2.0\n", s);
    CHECK(s.graphics.uiScale == doctest::Approx(2.0f));

    s.graphics.uiScale = 10.0f;
    EngineSettingsIO::Sanitize(s);
    CHECK(s.graphics.uiScale == doctest::Approx(3.0f)); // clamped high

    s.graphics.uiScale = 0.1f;
    EngineSettingsIO::Sanitize(s);
    CHECK(s.graphics.uiScale == doctest::Approx(0.5f)); // clamped low
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests`
Expected: **compile error** — `uiScale` is not a member of `EngineSettings::Graphics`.

- [ ] **Step 3: Add the field + reflection line + clamp**

In `src/engine/utils/EngineSettings.hpp`, add to `Graphics` (after `imguiViewports`):
```cpp
			float uiScale = 1.0f; // manual editor UI scale multiplier, on top of per-monitor DPI
```
Add to `ForEachSettingField` (grouped with the other `graphics.` lines):
```cpp
		f("graphics.uiScale", settings.graphics.uiScale);
```
In `src/engine/utils/EngineSettings.cpp`, add to `Sanitize` (after the `targetFps` clamp; `<algorithm>` is already included):
```cpp
		settings.graphics.uiScale = std::clamp(settings.graphics.uiScale, 0.5f, 3.0f);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build-vs2022-msvc --config Debug --target EngineTests && build-vs2022-msvc/tests/Debug/EngineTests.exe --test-case="uiScale*"`
Expected: **PASS**. Also run the full `EngineTests.exe` — the existing `Serialize`/`SerializeOverrides` cases must still pass (the new float field round-trips via the reflection list).

- [ ] **Step 5: Commit**

```bash
git add src/engine/utils/EngineSettings.hpp src/engine/utils/EngineSettings.cpp tests/utils/EngineSettingsTests.cpp
git commit -m "feat(settings): add graphics.uiScale (clamped 0.5-3.0)"
```

---

## Task 2: Enable per-monitor DPI + wire `uiScale`

**Files:**
- Modify: `src/engine/imgui/ImguiSubsystem.cpp` (Init: flags + remove DisplaySize override; add `SetUiScale`)
- Modify: `src/engine/imgui/ImguiSubsystem.hpp` (`SetUiScale` decl)
- Modify: `src/engine/AetherCore.hpp`, `src/engine/AetherCore.cpp` (`SetUiScale` forwarder)
- Modify: `src/engine/utils/SettingsService.cpp` (live-apply case)

No unit test (GPU/ImGui runtime). Verify by build + a hi-DPI runtime check (human).

- [ ] **Step 1: Enable the DPI config flags**

In `ImguiSubsystem::Init`, right after the existing `io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;` line, add:
```cpp
		// ImGui 1.92 per-monitor DPI: dynamic fonts re-rasterize crisply at monitor DPI
		// (ConfigDpiScaleFonts -> FontScaleDpi), and ImGui window geometry scales across
		// monitors (ConfigDpiScaleViewports). Complementary; no double font scaling.
		io.ConfigDpiScaleFonts = true;
		io.ConfigDpiScaleViewports = true;
```

- [ ] **Step 2: Stop overriding `io.DisplaySize`**

`imgui_impl_glfw`'s `NewFrame` sets the correct DPI-aware `DisplaySize` (logical) + `DisplayFramebufferScale`; the manual framebuffer-size override forces physical-as-logical and fights it.

In `ImguiSubsystem::Init`, **delete** this block (it sets `io.DisplaySize` from the framebuffer size; Init never calls `NewFrame`, so nothing consumes it):
```cpp
		if (auto window = services.TryGet<Window>())
		{
			const auto extent = window->GetFramebufferSize();
			io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
		}
```

In `ImguiSubsystem::BeginFrame`, **replace** the equivalent unconditional block:
```cpp
		if (auto window = services.TryGet<Window>())
		{
			const auto extent = window->GetFramebufferSize();
			io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
		}
```
with a fallback used only when the backends (and thus `ImGui_ImplGlfw_NewFrame`) aren't running, so `ImGui::NewFrame` never sees a zero `DisplaySize`:
```cpp
		if (!m_backendsInitialized)
		{
			if (auto window = services.TryGet<Window>())
			{
				const auto extent = window->GetFramebufferSize();
				io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
			}
		}
```

- [ ] **Step 3: Add `SetUiScale`**

In `ImguiSubsystem.hpp`, next to `SetViewportsEnabled`:
```cpp
		// Sets the manual editor UI-scale multiplier (producer thread). Composes with the
		// per-window DPI scale: GetFontSize == FontSizeBase * FontScaleMain * FontScaleDpi.
		void SetUiScale(float uiScale);
```
In `ImguiSubsystem.cpp` (near `SetViewportsEnabled`):
```cpp
	void ImguiSubsystem::SetUiScale(float uiScale)
	{
		if (!m_initialized)
		{
			return;
		}
		ImGui::GetStyle().FontScaleMain = std::clamp(uiScale, 0.5f, 3.0f);
	}
```
`ImguiSubsystem.cpp` already includes `<span>`/`<chrono>`; add `#include <algorithm>` for `std::clamp` if not present.

- [ ] **Step 4: `AetherCore::SetUiScale` forwarder**

In `AetherCore.hpp`, after `SetImguiViewportsEnabled`:
```cpp
		// Applies the manual editor UI scale at runtime (producer thread).
		void SetUiScale(float uiScale);
```
In `AetherCore.cpp`, after `SetImguiViewportsEnabled`:
```cpp
	void AetherCore::SetUiScale(float uiScale)
	{
		m_settings.graphics.uiScale = uiScale;
		if (m_imgui)
		{
			m_imgui->SetUiScale(uiScale);
		}
	}
```

- [ ] **Step 5: Live-apply from settings**

In `src/engine/utils/SettingsService.cpp`, add a branch in `ApplyLive` after the `graphics.imguiViewports` case:
```cpp
		else if (key == "graphics.uiScale")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetUiScale(m_values.graphics.uiScale);
			}
		}
```
(`ApplyAll` runs at startup after the engine is constructed, so the persisted `uiScale` is applied then; the `SettingsPanel` shows a float input automatically.)

- [ ] **Step 6: Build**

Run: `cmake --build build-vs2022-msvc --config Debug --target App EngineTests`
Expected: **success**, no warnings in the changed files.

- [ ] **Step 7: Runtime check (human, on a hi-DPI monitor)**

Run from the build root. Expected: on a hi-DPI monitor the fonts are **crisp** (not blurry-upscaled); editing `uiScale` in the Settings panel resizes the UI live. On a 100% monitor there is no visible change (DPI scale 1.0). Validation layers clean. *(If no hi-DPI monitor is available, confirm no regression at 100% and defer the visual check.)*

- [ ] **Step 8: Commit**

```bash
git add src/engine/imgui/ImguiSubsystem.hpp src/engine/imgui/ImguiSubsystem.cpp src/engine/AetherCore.hpp src/engine/AetherCore.cpp src/engine/utils/SettingsService.cpp
git commit -m "feat(imgui): per-monitor DPI (ConfigDpiScaleFonts/Viewports) + uiScale (FontScaleMain)"
```

---

## Task 3: "Match Panel" DPI-aware scene-viewport resolution mode

**Files:**
- Modify: `src/engine/rendering/RenderingSubsystem.hpp` (enum)
- Modify: `src/engine/rendering/RenderingSubsystem.cpp` (both resolve switches)
- Modify: `src/app/debug/ViewportPanel.cpp` (combo entry + drive the extent)

- [ ] **Step 1: Add the enum value**

In `RenderingSubsystem.hpp`, append to `SceneViewportResolutionMode` (keep existing values stable):
```cpp
	enum class SceneViewportResolutionMode : std::uint32_t
	{
		WindowNative = 0,
		Fixed720p,
		Fixed1080p,
		Fixed1440p,
		Custom,
		MatchPanel, // render at the Viewport panel's physical pixel size (logical x DPI)
	};
```

- [ ] **Step 2: Resolve `MatchPanel` to the requested extent**

`MatchPanel` renders at the extent the panel requests (driven in Step 3), carried in `customExtent`. In `RenderingSubsystem.cpp`, add a case to **both** switches — in `ResolveSceneViewportExtent` (the one using `m_sceneViewportSettings`, ~line 53) and `ResolveRequestedSceneViewportExtent` (using `m_requestedSceneViewportSettings`, ~line 90) — immediately before the `Custom` case, sharing its body:
```cpp
				case SceneViewportResolutionMode::MatchPanel:
				case SceneViewportResolutionMode::Custom:
					return clampExtent(m_sceneViewportSettings.customExtent); // ResolveScene...: m_sceneViewportSettings
```
(In `ResolveRequestedSceneViewportExtent` use `settings.customExtent` — the local `settings` copy that function already reads.)

- [ ] **Step 3: Drive the panel's physical extent + add the combo entry**

In `src/app/debug/ViewportPanel.cpp`, add `"Match Panel"` to the `resolutionModes` array:
```cpp
		const char* resolutionModes[] = {"Native", "720p", "1080p", "1440p", "Custom", "Match Panel"};
```
Then, immediately after the resolution `Combo` block (after the `Custom` popup handling, before `if (viewportSettingsChanged)`), drive the extent when in Match Panel mode. `available` (logical, computed earlier at ~line 360) × the panel window's DPI scale gives the physical pixel size:
```cpp
		if (viewportSettings.resolutionMode == SceneViewportResolutionMode::MatchPanel)
		{
			const float dpi = ImGui::GetWindowDpiScale();
			const auto physW = static_cast<std::uint32_t>(std::clamp(available.x * dpi, 64.0f, 8192.0f));
			const auto physH = static_cast<std::uint32_t>(std::clamp(available.y * dpi, 64.0f, 8192.0f));
			if (physW != viewportSettings.customExtent.width || physH != viewportSettings.customExtent.height)
			{
				viewportSettings.customExtent.width = physW;
				viewportSettings.customExtent.height = physH;
				viewportSettingsChanged = true;
			}
		}
```
The existing `if (viewportSettingsChanged) { ReleaseSceneViewportTexture(...); rendering.SetSceneViewportSettings(...); }` then rebuilds the scene target through the standard pending-rebuild path.

> Note: a continuous panel resize triggers a rebuild per size change (same cost model as the main swapchain rebuilding on window resize). Acceptable for an editor viewport; if it ever feels janky during a drag, add hysteresis (only commit when the delta exceeds a few px or the drag settles).

- [ ] **Step 4: Build**

Run: `cmake --build build-vs2022-msvc --config Debug --target App`
Expected: **success**. Confirm `ImGui::GetWindowDpiScale()` is available (it is — `imgui.h:474`) and `<algorithm>` is included in ViewportPanel.cpp for `std::clamp` (add if missing).

- [ ] **Step 5: Runtime check (human)**

Run from the build root. Select the Viewport panel's resolution combo → **Match Panel**. Expected: the scene renders at the panel's exact pixel size (1:1 crisp), tracks panel resize, and on a hi-DPI monitor renders at physical density. Tear the Viewport onto a different-DPI monitor → it re-resolves to that monitor's density. Validation clean.

- [ ] **Step 6: Commit**

```bash
git add src/engine/rendering/RenderingSubsystem.hpp src/engine/rendering/RenderingSubsystem.cpp src/app/debug/ViewportPanel.cpp
git commit -m "feat(viewport): DPI-aware Match Panel scene-resolution mode"
```

---

## Self-review notes (author)

- **Spec coverage:** auto per-monitor DPI + DisplaySize cleanup (T2 Steps 1-2), font lifetime (unchanged — already retained; verified in T2 Step 7), uiScale setting (T1) + wiring (T2 Steps 3-5), Match Panel mode (T3). The optional `ScaleAllSizes` padding tune is intentionally deferred to a hardware decision (spec §A/B) — not a task.
- **Type consistency:** `EngineSettings::Graphics::uiScale`; `ImguiSubsystem::SetUiScale`; `AetherCore::SetUiScale`; `SceneViewportResolutionMode::MatchPanel` — used consistently.
- **Ordering:** unlike the multi-viewport `SetViewportsEnabled`, `SetUiScale` is added to `ImguiSubsystem` (T2 Step 3) and referenced by `SettingsService` (T2 Step 5) within the same task, so the build stays green.
- **No new test file** (append to `EngineSettingsTests.cpp`), so no `tests/CMakeLists.txt` edit.
