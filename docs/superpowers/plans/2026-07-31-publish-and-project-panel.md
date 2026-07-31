# Publish Pipeline & Project Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce publishing to one button that cannot produce a broken build, and split the 961-line Project panel into a small Project panel and a new Build panel.

**Architecture:** `EditorProjectPublisher.cpp` (1023 lines, zero unit tests) splits into four focused TUs under `src/app/editor/publish/`. A pure `PublishPlan` resolves every path and label with no IO, so it is fully testable. `PublishVerify` holds every package invariant in one place and always runs. `PublishSteps` is an ordered list of uniform steps, so progress is computed rather than authored. The CMake `PackageGame` target and its two helper scripts are deleted, leaving one implementation of the packaging rules.

**Tech Stack:** C++23, CMake, doctest, Dear ImGui, toml++ (via `aether::TomlConfig`).

**Spec:** `docs/superpowers/specs/2026-07-31-publish-and-project-panel-design.md`

## Global Constraints

- Build and test with the MSVC preset: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug`, then run `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`.
- The full suite must stay green at every commit. Baseline at the time of writing: **635 test cases passing**.
- Adding a brand-new `src/app/**.cpp` requires `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc` before the target will pick it up.
- `src/app/project/` is filtered out of the `GameRuntime` target (see `src/app/CMakeLists.txt`). Anything the shipped runtime needs must live outside that directory. Everything in this plan lives under `src/app/editor/`, which is also excluded from `GameRuntime` — that is correct, because the publisher is editor-only.
- Commit style: plain imperative subject under ~72 chars, no `feat:`/`fix:` prefixes, no attribution lines. Body only when several changes are grouped, as a flat bullet list.
- Never use `catch (...)` around `TomlConfig::Load`; it returns `[[nodiscard]] bool` and does not throw.

## Spec Correction Applied By This Plan

The spec's decision 4 is "a non-Release build publishes and is labelled". Today `IsDebugCrtDll` (`EditorProjectPublisher.cpp:89`) makes a debug CRT DLL in the package a **hard publish failure**. It never fires right now only because the package template does not carry those DLLs. Once staging comes from the editor bundle, a Debug editor's bundle *does* carry them, so leaving the check as a gate would make every Debug publish fail — the opposite of decision 4.

**Resolution:** `IsDebugCrtDll` is deleted from verification. Shippability is reported from the compile-time build config (`AE_CONFIG_NAME` / `AE_DEV_TOOLING` in `src/engine/Defines.hpp`), which is authoritative and cannot be fooled by which DLLs happen to be present. Task 3 implements this.

**Caveat worth knowing:** the Microsoft debug CRT is not redistributable. A Debug publish is fine for handing to someone with a dev machine; it is not something to put on a storefront. The banner and report state the configuration so this is never ambiguous.

---

## File Structure

**Create:**
- `src/app/editor/publish/PublishPlan.hpp` / `.cpp` — pure path/label resolution. No IO.
- `src/app/editor/publish/PublishVerify.hpp` / `.cpp` — every package invariant.
- `src/app/editor/publish/PublishSteps.hpp` / `.cpp` — the ordered step list.
- `src/app/editor/publish/PublishReport.hpp` / `.cpp` — report text generation.
- `src/app/debug/BuildPanel.hpp` / `.cpp` — the new Build panel.
- `tests/app/PublishPlanTests.cpp`
- `tests/app/PublishVerifyTests.cpp`

**Modify:**
- `src/app/editor/EditorProjectPublisher.hpp` / `.cpp` — thin orchestration.
- `src/app/editor/EditorProjectActions.hpp` — drop `EditorProjectPublishOptions`, add `remediation`.
- `src/app/debug/ProjectPanel.hpp` / `.cpp` — shrink to project identity, folders, startup scene.
- `src/app/debug/EditorProjectManager.cpp` — action wiring.
- `src/app/layers/DebugLayer.cpp` — register `BuildPanel`.
- `src/app/CMakeLists.txt` — delete `PackageGame` and its command list.
- `CMakeLists.txt` — delete `AETHERCORE_PROJECT_DIR`.
- `tests/CMakeLists.txt` — add the new TUs.
- `resources/templates/ProjectSettings.toml` — drop `[publish]`.

**Delete:**
- `CMake/PrunePackageDevFiles.cmake`
- `CMake/VerifyAppPackage.cmake`

---

# Phase 1 — Publish pipeline

## Task 1: PublishPlan — pure path and label resolution

**Files:**
- Create: `src/app/editor/publish/PublishPlan.hpp`, `src/app/editor/publish/PublishPlan.cpp`
- Test: `tests/app/PublishPlanTests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `aether::app::EditorProjectContext` (from `src/app/editor/EditorProjectContext.hpp`; fields `root`, `name`, `projectFile`, `scenesDir`, `scriptsDir`).
- Produces: `aether::editor::PublishPlan`, `aether::editor::PublishEnvironment`, `MakePublishPlan(project, env)`, `CurrentPublishEnvironment()`, `SanitizePublishSegment(value, fallback)`. Tasks 2–6 all consume `PublishPlan`.

- [ ] **Step 1: Write the failing test**

Create `tests/app/PublishPlanTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "editor/EditorProjectContext.hpp"
#include "editor/publish/PublishPlan.hpp"

using namespace aether;

namespace
{
	app::EditorProjectContext MakeProject(const std::string& name)
	{
		app::EditorProjectContext project;
		project.root = std::filesystem::path("D:/work") / name;
		project.name = name;
		project.projectFile = project.root / "ProjectSettings.toml";
		project.scenesDir = project.root / "scenes";
		project.scriptsDir = project.root / "scripts";
		project.loaded = true;
		return project;
	}

	editor::PublishEnvironment MakeEnv()
	{
		editor::PublishEnvironment env;
		env.bundleDir = "D:/build/src/app/Debug";
		env.runtimeExeName = "AetherGame.exe";
		env.platformName = "Windows";
		env.configName = "Debug";
		env.shippableConfig = false;
		return env;
	}
} // namespace

TEST_CASE("MakePublishPlan derives the output directory from the project and platform") {
    const editor::PublishPlan plan = editor::MakePublishPlan(MakeProject("Whisper"), MakeEnv());

    CHECK(plan.outputDir == std::filesystem::path("D:/work/Whisper/Builds/Windows/Whisper"));
    CHECK(plan.productName == "Whisper");
    CHECK(plan.runtimeExeName == "AetherGame.exe");
    CHECK(plan.bundleDir == std::filesystem::path("D:/build/src/app/Debug"));
}

TEST_CASE("MakePublishPlan sanitises a project name that is not a valid path segment") {
    const editor::PublishPlan plan = editor::MakePublishPlan(MakeProject("My: Game?/v2"), MakeEnv());

    CHECK(plan.productName == "My_ Game__v2");
    CHECK(plan.outputDir.filename() == "My_ Game__v2");
}

TEST_CASE("MakePublishPlan falls back when the project name is empty or all separators") {
    app::EditorProjectContext project = MakeProject("Whisper");
    project.name = "///";

    const editor::PublishPlan plan = editor::MakePublishPlan(project, MakeEnv());

    CHECK(plan.productName == "AetherGame");
}

TEST_CASE("MakePublishPlan carries the build configuration through for labelling") {
    editor::PublishEnvironment env = MakeEnv();
    env.configName = "Release";
    env.shippableConfig = true;

    const editor::PublishPlan plan = editor::MakePublishPlan(MakeProject("Whisper"), env);

    CHECK(plan.configName == "Release");
    CHECK(plan.shippableConfig);
}

TEST_CASE("SanitizePublishSegment strips trailing dots and spaces that Windows rejects") {
    CHECK(editor::SanitizePublishSegment("Game. ", "fallback") == "Game");
    CHECK(editor::SanitizePublishSegment("   ", "fallback") == "fallback");
}
```

- [ ] **Step 2: Add the new TUs to the test build**

In `tests/CMakeLists.txt`, immediately after the `src/app/RuntimeProjectSettings.cpp` entry, add:

```cmake
    # Publish planning and package verification. Pure/near-pure halves of the publish
    # pipeline; the IO steps stay out of the test build.
    "${CMAKE_SOURCE_DIR}/src/app/editor/publish/PublishPlan.cpp"
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug`
Expected: FAIL to compile — `Cannot open include file: 'editor/publish/PublishPlan.hpp'`

- [ ] **Step 4: Write the header**

Create `src/app/editor/publish/PublishPlan.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// Everything about the machine and build doing the publishing. Separated from the
	// resolution below so the resolution is pure and can be tested without a real editor.
	struct PublishEnvironment
	{
		std::filesystem::path bundleDir;  // directory the running editor lives in
		std::string runtimeExeName;       // e.g. "AetherGame.exe"
		std::string platformName;         // e.g. "Windows"
		std::string configName;           // e.g. "Debug" - AE_CONFIG_NAME
		bool shippableConfig = false;     // false for Debug/RelWithDebInfo
	};

	// Every path and label a publish needs, resolved once up front. Nothing here is
	// configurable: the output location is derived from the project, which is what makes
	// "one button, no options" possible.
	struct PublishPlan
	{
		std::filesystem::path projectRoot;
		std::filesystem::path projectFile;
		std::filesystem::path scenesDir;
		std::filesystem::path scriptsDir;
		std::filesystem::path bundleDir;
		std::filesystem::path outputDir;
		std::string productName;
		std::string platformName;
		std::string runtimeExeName;
		std::string configName;
		bool shippableConfig = false;
	};

	// Replaces a path segment's illegal characters with '_' and trims the trailing dots
	// and spaces Windows silently drops. Returns `fallback` when nothing usable remains.
	[[nodiscard]] std::string SanitizePublishSegment(std::string value, std::string_view fallback);

	// Pure. No filesystem access - it only composes paths.
	[[nodiscard]] PublishPlan MakePublishPlan(const app::EditorProjectContext& project, const PublishEnvironment& environment);

	// Reads the running executable's directory and the compile-time build config.
	[[nodiscard]] PublishEnvironment CurrentPublishEnvironment();
} // namespace aether::editor
```

- [ ] **Step 5: Write the implementation**

Create `src/app/editor/publish/PublishPlan.cpp`:

```cpp
#include "editor/publish/PublishPlan.hpp"

#include "Defines.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/PlatformPaths.hpp"

namespace aether::editor
{
	namespace
	{
		constexpr std::string_view kProductFallback = "AetherGame";

		std::string HostPlatformName()
		{
#ifdef _WIN32
			return "Windows";
#elif defined(__APPLE__)
			return "macOS";
#elif defined(__linux__)
			return "Linux";
#else
			return "Desktop";
#endif
		}

		std::string HostRuntimeExeName()
		{
#ifdef AETHER_GAME_RUNTIME_EXE_NAME
			return AETHER_GAME_RUNTIME_EXE_NAME;
#elif defined(_WIN32)
			return "AetherGame.exe";
#else
			return "AetherGame";
#endif
		}
	} // namespace

	std::string SanitizePublishSegment(std::string value, const std::string_view fallback)
	{
		for (char& c: value)
		{
			const unsigned char ch = static_cast<unsigned char>(c);
			if (ch < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
			{
				c = '_';
			}
		}
		while (!value.empty() && (value.back() == ' ' || value.back() == '.'))
		{
			value.pop_back();
		}
		// All-separator or all-space names sanitise to underscores or nothing; neither is a
		// usable folder name, so fall back rather than create "___".
		if (value.find_first_not_of('_') == std::string::npos)
		{
			value.clear();
		}
		if (value.empty())
		{
			value = std::string(fallback);
		}
		return value;
	}

	PublishPlan MakePublishPlan(const app::EditorProjectContext& project, const PublishEnvironment& environment)
	{
		PublishPlan plan;
		plan.projectRoot = project.root;
		plan.projectFile = project.projectFile;
		plan.scenesDir = project.scenesDir;
		plan.scriptsDir = project.scriptsDir;
		plan.bundleDir = environment.bundleDir;
		plan.runtimeExeName = environment.runtimeExeName;
		plan.configName = environment.configName;
		plan.shippableConfig = environment.shippableConfig;
		plan.platformName = SanitizePublishSegment(environment.platformName, HostPlatformName());
		plan.productName = SanitizePublishSegment(project.name, kProductFallback);
		plan.outputDir = project.root / "Builds" / plan.platformName / plan.productName;
		return plan;
	}

	PublishEnvironment CurrentPublishEnvironment()
	{
		PublishEnvironment environment;
		environment.bundleDir = io::PlatformPaths::GetExecutableDir();
		environment.runtimeExeName = HostRuntimeExeName();
		environment.platformName = HostPlatformName();
		environment.configName = AE_CONFIG_NAME;
		// AE_DEV_TOOLING is 1 for Debug and RelWithDebInfo - exactly the configurations that
		// ship validation layers and unoptimised code.
		environment.shippableConfig = AE_DEV_TOOLING == 0;
		return environment;
	}
} // namespace aether::editor
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug` then `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe --test-case="*PublishPlan*,*SanitizePublishSegment*,*MakePublishPlan*"`
Expected: PASS, 5 test cases.

- [ ] **Step 7: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 640 test cases (635 baseline + 5).

- [ ] **Step 8: Commit**

```bash
git add src/app/editor/publish/PublishPlan.hpp src/app/editor/publish/PublishPlan.cpp tests/app/PublishPlanTests.cpp tests/CMakeLists.txt
git commit -m "Add PublishPlan, the pure half of publish path resolution"
```

---

## Task 2: PublishVerify — package invariants in one testable place

**Files:**
- Create: `src/app/editor/publish/PublishVerify.hpp`, `src/app/editor/publish/PublishVerify.cpp`
- Test: `tests/app/PublishVerifyTests.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `PublishPlan` (Task 1); `aether::io::PakBackend` (`io/PakBackend.hpp`); `aether::app::scene::SceneTextHasNoCameraSource` (declared in `scene/SceneWorkflow.hpp`).
- Produces: `PublishIssue { message, remediation }`, `VerifyPublishedPackage(packageDir, runtimeExeName)` returning `std::optional<PublishIssue>`, and the predicates `IsForbiddenPublishedFile`, `IsPrunablePublishedFile`, `IsAftermathRuntimeFile`, `IsPakSidecarFile`. Task 4 consumes the predicates; Task 5 consumes `VerifyPublishedPackage`.

- [ ] **Step 1: Write the failing test**

Create `tests/app/PublishVerifyTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "editor/publish/PublishVerify.hpp"
#include "io/FileUtil.hpp"

using namespace aether;

namespace
{
	// A package that satisfies every invariant except the ones a test deliberately breaks.
	// project.pak is a real pak built by the assetpack tests' helper only where a test needs
	// pak contents; the file-presence checks run before any pak is opened.
	struct FakePackage
	{
		std::filesystem::path root;

		explicit FakePackage(const std::string& name)
		{
			root = std::filesystem::temp_directory_path() / ("aethercore_publishverify_" + name);
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
			for (const char* rel: {"AetherGame.exe",
			                       "data/config/EngineSettings.toml",
			                       "data/engine.pak",
			                       "data/project.pak",
			                       "data/scripts/managed/AetherCore.dll",
			                       "data/scripts/managed/AetherCore.Interop.dll",
			                       "data/scripts/managed/AetherCore.Interop.deps.json",
			                       "data/scripts/managed/AetherCore.Interop.runtimeconfig.json",
			                       "data/scripts/managed/AetherGame.dll",
			                       "data/scripts/managed/AetherGame.deps.json"})
			{
				Write(rel, "x");
			}
		}

		~FakePackage()
		{
			std::error_code ec;
			std::filesystem::remove_all(root, ec);
		}

		void Write(const std::string& rel, const std::string& contents) const
		{
			const std::filesystem::path path = root / rel;
			REQUIRE(io::file_util::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(io::file_util::WriteText(path, contents).has_value());
		}

		void Remove(const std::string& rel) const
		{
			std::error_code ec;
			std::filesystem::remove(root / rel, ec);
		}
	};
} // namespace

TEST_CASE("VerifyPublishedPackage names the missing file") {
    const FakePackage package("missing");
    package.Remove("data/scripts/managed/AetherGame.dll");

    const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

    REQUIRE(issue.has_value());
    CHECK(issue->message.find("AetherGame.dll") != std::string::npos);
    CHECK_FALSE(issue->remediation.empty());
}

TEST_CASE("VerifyPublishedPackage rejects source files in the package") {
    const FakePackage package("source");
    package.Write("data/scripts/PlayerController.cs", "class X {}");

    const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

    REQUIRE(issue.has_value());
    CHECK(issue->message.find("PlayerController.cs") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects a packer sidecar") {
    const FakePackage package("sidecar");
    package.Write("data/project.pak.log", "packed 3 files");

    const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

    REQUIRE(issue.has_value());
    CHECK(issue->message.find("project.pak.log") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects the Aftermath DLL") {
    const FakePackage package("aftermath");
    package.Write("GFSDK_Aftermath_Lib.x64.dll", "binary");

    const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

    REQUIRE(issue.has_value());
    CHECK(issue->message.find("Aftermath") != std::string::npos);
}

TEST_CASE("VerifyPublishedPackage rejects the editor executable") {
    const FakePackage package("editor");
    package.Write("Editor.exe", "binary");

    const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

    REQUIRE(issue.has_value());
    CHECK(issue->message.find("Editor.exe") != std::string::npos);
}

// A debug CRT DLL is NOT a verification failure. Build configuration is reported from
// AE_CONFIG_NAME instead - see the spec correction in this plan's header.
TEST_CASE("VerifyPublishedPackage accepts a debug CRT DLL") {
    const FakePackage package("debugcrt");
    package.Write("msvcp140d.dll", "binary");
    package.Write("ucrtbased.dll", "binary");

    CHECK_FALSE(editor::VerifyPublishedPackage(package.root, "AetherGame.exe").has_value());
}

TEST_CASE("IsPrunablePublishedFile covers build leftovers but not shipped content") {
    CHECK(editor::IsPrunablePublishedFile("AetherGame.pdb"));
    CHECK(editor::IsPrunablePublishedFile("Engine.lib"));
    CHECK(editor::IsPrunablePublishedFile("Editor.ilk"));
    CHECK_FALSE(editor::IsPrunablePublishedFile("AetherGame.exe"));
    CHECK_FALSE(editor::IsPrunablePublishedFile("data/engine.pak"));
}

TEST_CASE("IsForbiddenPublishedFile covers source that must never ship") {
    CHECK(editor::IsForbiddenPublishedFile("Player.cs"));
    CHECK(editor::IsForbiddenPublishedFile("AetherGame.csproj"));
    CHECK_FALSE(editor::IsForbiddenPublishedFile("AetherGame.dll"));
}
```

- [ ] **Step 2: Add the TU to the test build**

In `tests/CMakeLists.txt`, immediately after the `PublishPlan.cpp` entry added in Task 1:

```cmake
    "${CMAKE_SOURCE_DIR}/src/app/editor/publish/PublishVerify.cpp"
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug`
Expected: FAIL to compile — `Cannot open include file: 'editor/publish/PublishVerify.hpp'`

- [ ] **Step 4: Write the header**

Create `src/app/editor/publish/PublishVerify.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aether::editor
{
	// What broke, and what to do about it. Kept separate so the UI can show the fix
	// without the user having to infer it from the failure text.
	struct PublishIssue
	{
		std::string message;
		std::string remediation;
	};

	// Build leftovers that are safe to delete from a package (the prune pass removes them).
	[[nodiscard]] bool IsPrunablePublishedFile(const std::filesystem::path& path);

	// Source and project files that must never ship (verification rejects them).
	[[nodiscard]] bool IsForbiddenPublishedFile(const std::filesystem::path& path);

	// Aftermath is dev-only and editor-gated; a shipped runtime never loads it.
	[[nodiscard]] bool IsAftermathRuntimeFile(const std::filesystem::path& path);

	// Packer sidecars have no runtime purpose and leak dev paths.
	[[nodiscard]] bool IsPakSidecarFile(const std::filesystem::path& path);

	// Every invariant a published package must satisfy. Returns nullopt when the package is
	// good. This always runs - a package that fails it is a failed publish, not a warning.
	[[nodiscard]] std::optional<PublishIssue> VerifyPublishedPackage(const std::filesystem::path& packageDir, std::string_view runtimeExeName);
} // namespace aether::editor
```

- [ ] **Step 5: Write the implementation**

Create `src/app/editor/publish/PublishVerify.cpp`:

```cpp
#include "editor/publish/PublishVerify.hpp"

#include <algorithm>
#include <exception>
#include <system_error>
#include <vector>

#include "io/FileUtil.hpp"
#include "io/PakBackend.hpp"
#include "scene/SceneWorkflow.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		std::string LowerAscii(std::string value)
		{
			std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		std::string EditorExecutableName()
		{
#ifdef AETHER_EDITOR_EXE_NAME
			return AETHER_EDITOR_EXE_NAME;
#elif defined(_WIN32)
			return "Editor.exe";
#else
			return "Editor";
#endif
		}

		std::vector<std::filesystem::path> RequiredPackageFiles(const std::string_view runtimeExeName)
		{
			return {
			        std::filesystem::path(runtimeExeName),
			        "data/config/EngineSettings.toml",
			        "data/engine.pak",
			        "data/project.pak",
			        "data/scripts/managed/AetherCore.dll",
			        "data/scripts/managed/AetherCore.Interop.dll",
			        "data/scripts/managed/AetherCore.Interop.deps.json",
			        "data/scripts/managed/AetherCore.Interop.runtimeconfig.json",
			        "data/scripts/managed/AetherGame.dll",
			        "data/scripts/managed/AetherGame.deps.json",
			};
		}

		std::optional<PublishIssue> VerifyShaders(const std::filesystem::path& enginePak)
		{
			try
			{
				const io::PakBackend pak(enginePak);
				const auto shaders = pak.Glob("shaders/**/*.spv", {});
				if (!shaders.has_value() || shaders->empty())
				{
					return PublishIssue{.message = "The published engine.pak contains no compiled shaders.",
					                    .remediation = "Run the Recompile Shaders action, then publish again."};
				}
			}
			catch (const std::exception& ex)
			{
				return PublishIssue{.message = "Could not read the published engine.pak: " + std::string(ex.what()),
					                .remediation = "Rebuild the editor so its engine.pak is regenerated, then publish again."};
			}
			return std::nullopt;
		}

		std::optional<PublishIssue> VerifyStartupScene(const std::filesystem::path& packageDir)
		{
			const std::filesystem::path settingsPath = packageDir / "data" / "config" / "EngineSettings.toml";
			auto text = io::file_util::ReadText(settingsPath);
			if (!text)
			{
				return PublishIssue{.message = "Could not read the published settings file.",
				                    .remediation = "Publish again; if it persists, check write permissions on the Builds folder."};
			}

			EngineSettings settings{};
			EngineSettingsIO::Apply(*text, settings);
			if (settings.app.startupScene.empty())
			{
				return PublishIssue{.message = "The published settings have no startup scene.",
				                    .remediation = "Set a startup scene in the Project panel, then publish again."};
			}
			if (!settings.app.autoplay)
			{
				return PublishIssue{.message = "The published settings have autoplay disabled, so the game would boot in edit mode.",
				                    .remediation = "This is an engine bug - the publish bake must force autoplay. Report it."};
			}

			const std::filesystem::path projectPak = packageDir / "data" / "project.pak";
			try
			{
				const io::PakBackend pak(projectPak);
				const std::string sceneVirtualPath = "scenes/" + settings.app.startupScene + ".scene.toml";
				if (!pak.Exists(sceneVirtualPath))
				{
					return PublishIssue{.message = "Startup scene '" + settings.app.startupScene + "' is not in the published project.pak.",
					                    .remediation = "Confirm the scene exists in the project's scenes folder, then publish again."};
				}
				if (const auto bytes = pak.Read(sceneVirtualPath); bytes.has_value())
				{
					const std::string_view sceneToml(reinterpret_cast<const char*>(bytes->data()), bytes->size());
					if (app::scene::SceneTextHasNoCameraSource(sceneToml))
					{
						// Non-fatal: a script may create a camera at runtime.
						AE_WARN(LogCategory::App, "Published startup scene '{}' has no main camera.", settings.app.startupScene);
					}
				}
			}
			catch (const std::exception& ex)
			{
				return PublishIssue{.message = "Could not read the published project.pak: " + std::string(ex.what()),
				                    .remediation = "Publish again to rebuild it."};
			}
			return std::nullopt;
		}
	} // namespace

	bool IsPrunablePublishedFile(const std::filesystem::path& path)
	{
		const std::string ext = LowerAscii(path.extension().generic_string());
		return ext == ".pdb" || ext == ".lib" || ext == ".exp" || ext == ".ilk";
	}

	bool IsForbiddenPublishedFile(const std::filesystem::path& path)
	{
		const std::string ext = LowerAscii(path.extension().generic_string());
		return ext == ".cs" || ext == ".csproj" || ext == ".vcxproj";
	}

	bool IsAftermathRuntimeFile(const std::filesystem::path& path)
	{
		return LowerAscii(path.stem().generic_string()).starts_with("gfsdk_aftermath");
	}

	bool IsPakSidecarFile(const std::filesystem::path& path)
	{
		const std::string name = LowerAscii(path.filename().generic_string());
		return name.ends_with(".pak.log") || name.ends_with(".pak.manifest");
	}

	std::optional<PublishIssue> VerifyPublishedPackage(const std::filesystem::path& packageDir, const std::string_view runtimeExeName)
	{
		for (const std::filesystem::path& rel: RequiredPackageFiles(runtimeExeName))
		{
			if (!io::file_util::Exists(packageDir / rel))
			{
				return PublishIssue{.message = "The published build is missing " + rel.generic_string() + ".",
				                    .remediation = "Publish again; if it persists, rebuild the editor so its bundle is complete."};
			}
		}

		const std::string editorExe = EditorExecutableName();
		if (editorExe != runtimeExeName && io::file_util::Exists(packageDir / editorExe))
		{
			return PublishIssue{.message = "The published build contains the editor executable (" + editorExe + ").",
			                    .remediation = "This is an engine bug - staging must copy only the runtime. Report it."};
		}

		std::error_code ec;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(packageDir, ec))
		{
			if (ec)
			{
				return PublishIssue{.message = "Could not inspect the published folder: " + ec.message(),
				                    .remediation = "Close anything using the Builds folder and publish again."};
			}
			if (!entry.is_regular_file(ec))
			{
				continue;
			}
			const std::filesystem::path& path = entry.path();
			if (IsForbiddenPublishedFile(path) || IsPrunablePublishedFile(path))
			{
				return PublishIssue{.message = "The published build contains a dev/source file: " + path.filename().generic_string() + ".",
				                    .remediation = "This is an engine bug - the prune step should have removed it. Report it."};
			}
			if (IsAftermathRuntimeFile(path))
			{
				return PublishIssue{.message = "The published build contains NVIDIA Aftermath (" + path.filename().generic_string() + "), which is dev-only.",
				                    .remediation = "This is an engine bug - staging must skip it. Report it."};
			}
			if (IsPakSidecarFile(path))
			{
				return PublishIssue{.message = "The published build contains a packer sidecar: " + path.filename().generic_string() + ".",
				                    .remediation = "This is an engine bug - the prune step should have removed it. Report it."};
			}
		}

		if (auto issue = VerifyShaders(packageDir / "data" / "engine.pak"))
		{
			return issue;
		}
		return VerifyStartupScene(packageDir);
	}
} // namespace aether::editor
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug` then `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe --test-case="*VerifyPublishedPackage*,*IsPrunablePublishedFile*,*IsForbiddenPublishedFile*"`

Expected: the five file-presence/content cases PASS. The "accepts a debug CRT DLL" case will FAIL at the shader check, because the fake `data/engine.pak` is the text `x`, not a real pak. Fix the test by having that case assert only that no *debug-CRT* issue is reported:

```cpp
TEST_CASE("VerifyPublishedPackage accepts a debug CRT DLL") {
    const FakePackage package("debugcrt");
    package.Write("msvcp140d.dll", "binary");
    package.Write("ucrtbased.dll", "binary");

    const auto issue = editor::VerifyPublishedPackage(package.root, "AetherGame.exe");

    // It will still fail on the stub engine.pak, but never because of the CRT DLLs.
    if (issue.has_value())
    {
        CHECK(issue->message.find("msvcp140d") == std::string::npos);
        CHECK(issue->message.find("ucrtbased") == std::string::npos);
    }
}
```

Re-run and confirm all 8 cases PASS.

- [ ] **Step 7: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 648 test cases.

- [ ] **Step 8: Commit**

```bash
git add src/app/editor/publish/PublishVerify.hpp src/app/editor/publish/PublishVerify.cpp tests/app/PublishVerifyTests.cpp tests/CMakeLists.txt
git commit -F - <<'EOF'
Move package verification into its own tested unit

- Add PublishVerify with every published-package invariant in one place
- Stop failing a publish on debug CRT DLLs; build configuration is reported
  from AE_CONFIG_NAME instead, which a Debug publish can no longer contradict
EOF
```

---

## Task 3: PublishReport — report text from the plan

**Files:**
- Create: `src/app/editor/publish/PublishReport.hpp`, `src/app/editor/publish/PublishReport.cpp`

**Interfaces:**
- Consumes: `PublishPlan` (Task 1).
- Produces: `PublishReport { bool ok; std::string summary; std::string text; }` and `BuildPublishReport(const PublishPlan&)`. Task 5 consumes both.

- [ ] **Step 1: Write the header**

Create `src/app/editor/publish/PublishReport.hpp`:

```cpp
#pragma once

#include <string>

namespace aether::editor
{
	struct PublishPlan;

	// Describes what actually shipped. Written next to the package as publish-report.txt so
	// a build can be identified after it leaves the machine that made it.
	struct PublishReport
	{
		bool ok = false;
		std::string summary; // one line, shown in the Build panel
		std::string text;    // full report file contents
	};

	[[nodiscard]] PublishReport BuildPublishReport(const PublishPlan& plan);
} // namespace aether::editor
```

- [ ] **Step 2: Write the implementation**

Create `src/app/editor/publish/PublishReport.cpp`. Port the size-walking and `HumanBytes` logic from the existing `BuildPublishReport` at `src/app/editor/EditorProjectPublisher.cpp:659-737`, keeping its sections (Product / Platform / Runtime / Startup scene / Package / Key payload / Largest files) and adding a configuration line:

```cpp
#include "editor/publish/PublishReport.hpp"

#include <algorithm>
#include <sstream>
#include <system_error>
#include <vector>

#include "editor/publish/PublishPlan.hpp"
#include "io/FileUtil.hpp"
#include "project/ProjectStartupScene.hpp"

namespace aether::editor
{
	namespace
	{
		std::string HumanBytes(const std::uintmax_t bytes)
		{
			constexpr double kKib = 1024.0;
			std::ostringstream out;
			out.setf(std::ios::fixed);
			out.precision(2);
			const auto value = static_cast<double>(bytes);
			if (value < kKib)
			{
				return std::to_string(bytes) + " B";
			}
			if (value < kKib * kKib)
			{
				out << value / kKib << " KB";
			}
			else
			{
				out << value / (kKib * kKib) << " MB";
			}
			return out.str();
		}

		struct PackagedFile
		{
			std::string rel;
			std::uintmax_t bytes = 0;
		};
	} // namespace

	PublishReport BuildPublishReport(const PublishPlan& plan)
	{
		PublishReport report;
		std::error_code ec;
		std::vector<PackagedFile> files;
		std::uintmax_t total = 0;
		for (const auto& entry: std::filesystem::recursive_directory_iterator(plan.outputDir, ec))
		{
			if (ec)
			{
				return report;
			}
			if (!entry.is_regular_file(ec))
			{
				continue;
			}
			const std::uintmax_t size = entry.file_size(ec);
			if (ec)
			{
				continue;
			}
			total += size;
			files.push_back({std::filesystem::relative(entry.path(), plan.outputDir, ec).generic_string(), size});
		}
		std::ranges::sort(files, [](const PackagedFile& a, const PackagedFile& b) { return a.bytes > b.bytes; });

		const std::string startupScene = app::ReadProjectStartupScene(plan.projectFile);

		std::ostringstream text;
		text << "AetherCore Publish Report\n=========================\n\n";
		text << "Product:       " << plan.productName << "\n";
		text << "Platform:      " << plan.platformName << "\n";
		text << "Runtime:       " << plan.runtimeExeName << "\n";
		text << "Configuration: " << plan.configName << (plan.shippableConfig ? "\n" : "  (testing only - not a shippable build)\n");
		text << "Startup scene: " << (startupScene.empty() ? "(unset)" : startupScene) << "\n\n";
		text << "Package\n-------\n";
		text << "Total size:    " << HumanBytes(total) << "\n";
		text << "Files:         " << files.size() << "\n\n";
		text << "Largest files\n-------------\n";
		for (std::size_t i = 0; i < files.size() && i < 8; ++i)
		{
			text << "  " << HumanBytes(files[i].bytes) << "  " << files[i].rel << "\n";
		}

		report.ok = true;
		report.text = text.str();
		report.summary = "Published " + plan.productName + " (" + HumanBytes(total) + ", " + std::to_string(files.size()) + " files, " + plan.configName + ").";
		return report;
	}
} // namespace aether::editor
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target Editor --config Debug`
Expected: `0 Error(s)`

- [ ] **Step 4: Commit**

```bash
git add src/app/editor/publish/PublishReport.hpp src/app/editor/publish/PublishReport.cpp
git commit -m "Add PublishReport and record the build configuration in it"
```

---

## Task 4: PublishSteps — the ordered pipeline

**Files:**
- Create: `src/app/editor/publish/PublishSteps.hpp`, `src/app/editor/publish/PublishSteps.cpp`

**Interfaces:**
- Consumes: `PublishPlan` (Task 1), `PublishIssue` + predicates + `VerifyPublishedPackage` (Task 2), `BuildPublishReport` (Task 3), `app::ValidateProjectStartupScene` (`project/ProjectStartupScene.hpp`), `assetpipeline::PackProject`, `app::scene::CookProjectBinaries`, `io::RunProcessToLog`.
- Produces: `PublishToolchain`, `MakePublishToolchain()`, `PublishContext`, `StepResult`, `PublishStep`, `PublishStepList()`. Task 5 consumes all of them.

- [ ] **Step 1: Write the header**

Create `src/app/editor/publish/PublishSteps.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace aether::editor
{
	struct PublishPlan;

	// Where the external tools live. Populated from the AETHER_* compile definitions.
	struct PublishToolchain
	{
		std::filesystem::path dotnetExe;
		std::string managedConfig;
		std::string managedConfigDir;
		std::filesystem::path managedSdkProject;
	};

	[[nodiscard]] PublishToolchain MakePublishToolchain();

	// Mutable state threaded through the steps: what earlier steps produced.
	struct PublishContext
	{
		std::filesystem::path packedProjectPak;
		std::string reportSummary;
	};

	struct StepResult
	{
		bool ok = true;
		std::string message;
		std::string remediation;
	};

	struct PublishStep
	{
		std::string_view name; // shown as the progress stage
		StepResult (*run)(const PublishPlan&, PublishContext&, const PublishToolchain&);
	};

	// The pipeline, in order. Progress is derived from position in this list, so adding a
	// step never requires retuning a progress fraction.
	[[nodiscard]] std::span<const PublishStep> PublishStepList();
} // namespace aether::editor
```

- [ ] **Step 2: Write the implementation**

Create `src/app/editor/publish/PublishSteps.cpp`. Each step is a free function with the uniform signature; port the bodies from the existing `PublishProject` in `EditorProjectPublisher.cpp:858-1010`, with these changes:

- `StageRuntime` replaces both `CopyRuntimeFromExecutableDir` and `CopyShippedDataPayload` with **one explicit payload list** copied from `plan.bundleDir`: the runtime executable, every `.dll`/`.so`/`.dylib` in the bundle root except Aftermath, `data/engine.pak`, `data/config/EngineSettings.toml`, and `data/scripts/managed/`. No `packageTemplateDir`.
- `BakeSettings` calls `EngineSettingsIO::LoadLayered(publishedSettingsPath, plan.projectFile)`, forces `loaded.base.app.autoplay = true`, and validates with `app::ValidateProjectStartupScene(plan.scenesDir, loaded.base.app.startupScene, error)` before writing.
- `Prune` uses `IsPrunablePublishedFile` and `IsPakSidecarFile` from Task 2.
- `Verify` calls `VerifyPublishedPackage(plan.outputDir, plan.runtimeExeName)` and maps a returned `PublishIssue` onto `StepResult`.
- No step writes outside `plan.outputDir`. In particular, nothing copies `project.pak` into the editor's own `data/` directory — that behaviour stays in Pack only.

The step list:

```cpp
	std::span<const PublishStep> PublishStepList()
	{
		static constexpr PublishStep kSteps[] = {
		        {"Validating project", &ValidateProject},
		        {"Cleaning output", &CleanOutput},
		        {"Packing project assets", &PackProjectAssets},
		        {"Staging game runtime", &StageRuntime},
		        {"Baking game settings", &BakeSettings},
		        {"Building game scripts", &BuildScripts},
		        {"Removing dev files", &PruneDevFiles},
		        {"Verifying package", &VerifyPackage},
		        {"Writing report", &WriteReport},
		};
		return kSteps;
	}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target Editor --config Debug`
Expected: `0 Error(s)`

- [ ] **Step 4: Commit**

```bash
git add src/app/editor/publish/PublishSteps.hpp src/app/editor/publish/PublishSteps.cpp
git commit -F - <<'EOF'
Turn publish into an ordered list of uniform steps

- Replace the two divergent staging paths with one step and an explicit payload list
- Derive progress from position in the step list instead of hand-tuned fractions
- Stop publish writing project.pak into the editor's own data directory
EOF
```

---

## Task 5: Rewire EditorProjectPublisher onto the steps and delete the options

**Files:**
- Modify: `src/app/editor/EditorProjectPublisher.hpp`, `src/app/editor/EditorProjectPublisher.cpp`, `src/app/editor/EditorProjectActions.hpp`, `src/app/debug/EditorProjectManager.cpp`, `src/app/debug/ProjectPanel.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1–4.
- Produces: `PackProject(project)`, `PublishProject(project, progress)`, `PlanPublish(project)`. `EditorProjectActionResult` gains `std::string remediation`. `EditorProjectPublishOptions` and `EditorProjectPublishConfig` cease to exist. Tasks 6–10 consume the new signatures.

- [ ] **Step 1: Add remediation to the action result**

In `src/app/editor/EditorProjectActions.hpp`, extend the result and delete the options struct:

```cpp
	struct EditorProjectActionResult
	{
		bool succeeded = false;
		std::string message;
		std::string remediation; // what to do about a failure; empty on success
		std::filesystem::path outputPath;
	};
```

Delete `struct EditorProjectPublishOptions` entirely, and change the action signature:

```cpp
		std::function<EditorProjectActionResult(const app::EditorProjectContext&, const EditorProjectPublishProgress&)> publishProject;
```

- [ ] **Step 2: Rewrite the publisher header**

`src/app/editor/EditorProjectPublisher.hpp` becomes:

```cpp
#pragma once

#include "editor/EditorProjectActions.hpp"
#include "editor/publish/PublishPlan.hpp"

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// The plan a publish would use. The Build panel calls this to show the destination and
	// the build configuration before anything runs.
	[[nodiscard]] PublishPlan PlanPublish(const app::EditorProjectContext& project);

	[[nodiscard]] EditorProjectActionResult PackProject(const app::EditorProjectContext& project);

	[[nodiscard]] EditorProjectActionResult PublishProject(const app::EditorProjectContext& project, const EditorProjectPublishProgress& progress = {});
} // namespace aether::editor
```

- [ ] **Step 3: Rewrite the publisher body as orchestration**

`src/app/editor/EditorProjectPublisher.cpp` keeps only `PlanPublish`, `PackProject` (with its editor-pak sync retained) and:

```cpp
	EditorProjectActionResult PublishProject(const app::EditorProjectContext& project, const EditorProjectPublishProgress& progress)
	{
		const PublishPlan plan = PlanPublish(project);
		const PublishToolchain toolchain = MakePublishToolchain();
		PublishContext context;

		const std::span<const PublishStep> steps = PublishStepList();
		for (std::size_t i = 0; i < steps.size(); ++i)
		{
			if (progress)
			{
				progress(static_cast<float>(i) / static_cast<float>(steps.size()), steps[i].name);
			}
			const StepResult result = steps[i].run(plan, context, toolchain);
			if (!result.ok)
			{
				AE_ERROR(LogCategory::App, "Publish failed at '{}': {}", steps[i].name, result.message);
				return {.succeeded = false, .message = result.message, .remediation = result.remediation, .outputPath = plan.outputDir};
			}
		}
		if (progress)
		{
			progress(1.0f, "Published");
		}
		return {.succeeded = true, .message = context.reportSummary, .outputPath = plan.outputDir};
	}
```

Delete from this TU: `ResolvePublishRoot`, `ResolvePublishDirectory`, `SanitizePathSegment`, `FindCMakePackageDirectory`, `CopyRuntimeFromExecutableDir`, `CopyShippedDataPayload`, `BakePublishedEngineSettings`, `VerifyPublishedGame`, `VerifyPublishedGameShaders`, `VerifyPublishedStartupScene`, `ClassifyPublishedFile`, `PrunePublishedDevFiles`, `IsAftermathRuntimeFile`, `IsDebugCrtDll`, `IsPakSidecarFile`, `BuildPublishReport`, `HumanBytes`, `MakeDefaultEditorProjectPublishConfig`, `MakeDefaultEditorProjectPublishOptions`, and `struct EditorProjectPublishConfig`.

- [ ] **Step 4: Update the action wiring**

In `src/app/debug/EditorProjectManager.cpp`, replace the two lambdas at lines 176-183:

```cpp
		m_actions.packProject = [](const app::EditorProjectContext& project)
		{
			return PackProject(project);
		};
		m_actions.publishProject = [](const app::EditorProjectContext& project, const EditorProjectPublishProgress& progress)
		{
			return PublishProject(project, progress);
		};
```

Also update the `rebuildEnginePak` lambda at lines 207-211, which used `MakeDefaultEditorProjectPublishConfig().executableDir`:

```cpp
			m_actions.rebuildEnginePak = []
			{
				return BakeEnginePak(io::PlatformPaths::GetExecutableDir() / "data" / "engine.pak");
			};
```

- [ ] **Step 5: Make ProjectPanel compile against the new API**

This is a temporary edit; Tasks 9–10 rewrite the panel. In `src/app/debug/ProjectPanel.cpp`, delete `LoadPublishSettings`, `SavePublishSettings`, `ResetPublishSettings` and `DrawPublishDialog` along with their declarations in `ProjectPanel.hpp` and all nine `m_publish*` option members. Replace the Publish button body with a direct call:

```cpp
					const char* publishLabel = ICON_FA_ROCKET " Publish";
					row.Item(publishLabel);
					ImGui::BeginDisabled(!actions->publishProject || m_publishFuture.valid());
					if (chrome::PrimaryButton(publishLabel))
					{
						m_publishTask = std::make_shared<PublishTask>();
						const auto publishAction = actions->publishProject;
						const app::EditorProjectContext projectCopy = *project;
						const std::shared_ptr<PublishTask> task = m_publishTask;
						m_publishFuture = std::async(std::launch::async,
						        [publishAction, projectCopy, task]()
						        {
							        return publishAction(projectCopy,
							                [task](const float completion, const std::string_view stage)
							                {
								                task->completion.store(completion, std::memory_order_release);
								                const std::scoped_lock lock(task->mutex);
								                task->stage = std::string(stage);
							                });
						        });
					}
					ImGui::EndDisabled();
```

- [ ] **Step 6: Build the editor**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target Editor GameRuntime --config Debug`
Expected: `0 Error(s)`

- [ ] **Step 7: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 648 test cases.

- [ ] **Step 8: Publish Whisper end to end**

Launch the editor:

```bash
D:/AetherCore/build/vs2022-msvc/src/app/Debug/Editor.exe --project D:\AetherCore\projects\Whisper --no-validation
```

Click Publish in the Project panel's Build tab. Then run the result:

```bash
D:/AetherCore/projects/Whisper/Builds/Windows/Whisper/AetherGame.exe
```

Expected: the Whisper title screen, and `Startup scene 'Title' loaded (32 entities)` in the console output. `publish-report.txt` in the package must show `Configuration: Debug  (testing only - not a shippable build)`.

- [ ] **Step 9: Commit**

```bash
git add src/app/editor src/app/debug/EditorProjectManager.cpp src/app/debug/ProjectPanel.cpp src/app/debug/ProjectPanel.hpp
git commit -F - <<'EOF'
Make publish one action with no options

- Run publish as an ordered step list over a resolved PublishPlan
- Delete EditorProjectPublishOptions and the publish settings dialog
- Derive the output path from the project instead of reading it from a config file
- Carry remediation text on a failed action so the UI can say what to do
EOF
```

---

## Task 6: Delete PackageGame and its helper scripts

**Files:**
- Modify: `src/app/CMakeLists.txt`, `CMakeLists.txt`
- Delete: `CMake/PrunePackageDevFiles.cmake`, `CMake/VerifyAppPackage.cmake`

- [ ] **Step 1: Remove the target and its command list**

In `src/app/CMakeLists.txt`, delete the `_aether_package_dir` / `_aether_package_commands` block and the `add_custom_target(PackageGame ...)` call at the end of it. Leave `EngineAssetsPak` and `StageAppBundle` untouched — publish now depends on `StageAppBundle`'s output.

In the same file, remove `PackageGame` from the folder-grouping call in the root `CMakeLists.txt:73`:

```cmake
aethercore_set_folder("Build"        ManagedAssemblies CompileShaders clean-all dead-strip-report uninstall)
```

- [ ] **Step 2: Remove the project cache variable**

In `CMakeLists.txt`, delete the `AETHERCORE_PROJECT_DIR` block at lines 24-26 and the warning that references it in `src/app/CMakeLists.txt`.

- [ ] **Step 3: Delete the helper scripts**

```bash
git rm CMake/PrunePackageDevFiles.cmake CMake/VerifyAppPackage.cmake
```

- [ ] **Step 4: Verify nothing still references them**

Run: `grep -rn "PackageGame\|VerifyAppPackage\|PrunePackageDevFiles\|AETHERCORE_PROJECT_DIR" --include=*.txt --include=*.cmake --include=*.cpp --include=*.hpp --include=*.md . | grep -v "^./build/" | grep -v "^./docs/superpowers"`
Expected: no output.

- [ ] **Step 5: Reconfigure and build everything**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target Editor GameRuntime EngineTests --config Debug`
Expected: `0 Error(s)`, and no CMake warning about a missing package target.

- [ ] **Step 6: Commit**

```bash
git add -A CMakeLists.txt src/app/CMakeLists.txt CMake
git commit -F - <<'EOF'
Delete the PackageGame target and its duplicate packaging rules

- Remove PackageGame, PrunePackageDevFiles.cmake and VerifyAppPackage.cmake
- Remove AETHERCORE_PROJECT_DIR, which baked a build-time project into a package
EOF
```

---

## Task 7: Drop [publish] from the project file template and strip it on write

**Files:**
- Modify: `resources/templates/ProjectSettings.toml`, `src/app/project/ProjectStartupScene.cpp`
- Test: `tests/app/ProjectStartupSceneTests.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/app/ProjectStartupSceneTests.cpp`:

```cpp
// [publish] was build-machine configuration (including an absolute output path) living in
// a file shared through git. Publish no longer reads it, so any write drops it.
TEST_CASE("WriteProjectStartupScene strips a stale [publish] section") {
    const TempProject project("publishstrip");
    REQUIRE(io::file_util::WriteText(project.projectFile,
                                     "[app]\nstartupscene = \"Title\"\n\n"
                                     "[publish]\noutputroot = \"D:\\\\somewhere\\\\Builds\"\ncleanoutput = true\n\n"
                                     "[project]\nname = \"Demo\"\n")
                .has_value());

    std::string error;
    REQUIRE(app::WriteProjectStartupScene(project.projectFile, "Arena", error));

    const std::string text = project.ProjectFileText();
    CHECK(text.find("[publish]") == std::string::npos);
    CHECK(text.find("outputroot") == std::string::npos);
    CHECK(app::ReadProjectStartupScene(project.projectFile) == "Arena");
    CHECK(text.find("name = \"Demo\"") != std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug` then `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe --test-case="*strips a stale*"`
Expected: FAIL — `[publish]` is still present.

- [ ] **Step 3: Strip the section on write**

`TomlConfig` has no key-removal API, so filter while copying. In `src/app/project/ProjectStartupScene.cpp`, add a `Remove` to `TomlConfig` is out of scope; instead, after `config.Load(*text)` succeeds in `WriteProjectStartupScene`, add:

```cpp
		// [publish] was build-machine configuration in a file shared through git. Nothing
		// reads it any more, so drop it the first time we write the file.
		for (const char* key: {"publish.productname", "publish.platformname", "publish.outputroot",
		                       "publish.cleanoutput", "publish.buildscripts", "publish.usepackagetemplate",
		                       "publish.verifyoutput", "publish.synceditorpak", "publish.openafter"})
		{
			config.Erase(key);
		}
```

Add the missing method to `src/engine/utils/TomlConfig.hpp` / `.cpp`:

```cpp
		// Removes a key if present. Used to drop settings that have been retired, so a
		// project file heals on the next write instead of carrying dead keys forever.
		void Erase(std::string_view key);
```

```cpp
	void TomlConfig::Erase(std::string_view key)
	{
		if (m_values.erase(text::ToLowerAscii(std::string(key))) > 0)
		{
			m_dirty = true;
		}
	}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target EngineTests --config Debug` then `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe --test-case="*strips a stale*"`
Expected: PASS

- [ ] **Step 5: Remove [publish] from the template**

In `resources/templates/ProjectSettings.toml`, delete the entire `[publish]` block (lines 17-26).

- [ ] **Step 6: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 649 test cases.

- [ ] **Step 7: Commit**

```bash
git add resources/templates/ProjectSettings.toml src/app/project/ProjectStartupScene.cpp src/engine/utils/TomlConfig.hpp src/engine/utils/TomlConfig.cpp tests/app/ProjectStartupSceneTests.cpp
git commit -F - <<'EOF'
Retire the [publish] section from project files

- Drop it from the new-project template
- Strip it on the next write so existing project files heal
- Add TomlConfig::Erase for retiring keys
EOF
```

---

# Phase 2 — Panels

## Task 8: Extract BuildPanel with the build actions

**Files:**
- Create: `src/app/debug/BuildPanel.hpp`, `src/app/debug/BuildPanel.cpp`
- Modify: `src/app/layers/DebugLayer.cpp`

**Interfaces:**
- Consumes: `EditorProjectActions` (Task 5 signatures), `PlanPublish` (Task 5), `chrome::` button helpers from `debug/EditorChrome.hpp`.
- Produces: a `DebugPanel` named `"Build"`.

- [ ] **Step 1: Write the header**

Create `src/app/debug/BuildPanel.hpp`:

```cpp
#pragma once

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "debug/DebugPanel.hpp"
#include "editor/EditorProjectActions.hpp"

namespace aether::editor
{
	// Everything that turns the open project into something runnable: pack, shaders,
	// engine pak, the C# debugger, and publish. Split out of ProjectPanel, which is about
	// what the project IS rather than what you build from it.
	class BuildPanel final : public DebugPanel
	{
	public:
		~BuildPanel() override;

		std::string_view GetName() const override
		{
			return "Build";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnImGui(app::LayerContext& context) override;

	private:
		struct PublishTask
		{
			std::atomic<float> completion = 0.0f;
			std::mutex mutex;
			std::string stage;
		};

		// One status for the whole panel. Four parallel status/succeeded pairs was the
		// old shape, and it meant a stale pack message sat under a fresh publish failure.
		struct ActionStatus
		{
			std::string message;
			std::string remediation;
			bool ok = false;
			std::filesystem::path output;
		};

		void DrawConfigBanner(const app::EditorProjectContext& project);
		void DrawActionRow(app::LayerContext& context, const app::EditorProjectContext& project);
		void DrawScriptDebuggerRow(app::LayerContext& context);
		void DrawStatus();
		void PollPublish();

		ActionStatus m_status;
		std::filesystem::path m_visualStudioInstall;
		std::filesystem::path m_lastPublishPath;
		bool m_lastPublishSucceeded = false;
		std::future<EditorProjectActionResult> m_publishFuture;
		std::shared_ptr<PublishTask> m_publishTask;
	};
} // namespace aether::editor
```

- [ ] **Step 2: Write the implementation**

Create `src/app/debug/BuildPanel.cpp`. Move these from `ProjectPanel.cpp` unchanged in behaviour: the Visual Studio picker (lines 805-846), the Pack / Rebuild Engine Pak / Recompile Shaders buttons (lines 851-903), and the `LaunchGameBuild` / `OpenFolderInShell` helpers. Then:

- `DrawConfigBanner` calls `editor::PlanPublish(project)` and, when `!plan.shippableConfig`, draws in `chrome::kWarning`: `"<config> build - for testing, not for shipping"`, followed by the destination `plan.outputDir` in `chrome::kMuted`.
- Publish is a single `chrome::PrimaryButton` that launches the async task exactly as in Task 5 Step 5.
- While `m_publishFuture.valid()`, draw `ImGui::ProgressBar(task->completion.load(std::memory_order_acquire))` with the stage string beneath it. No modal.
- `PollPublish` picks the future up when ready and fills `m_status` from the result, including `remediation`.
- `DrawStatus` renders `m_status.message` in `chrome::kSuccess`/`chrome::kError`, and `m_status.remediation` beneath it in `chrome::kMuted` when non-empty.
- After a successful publish, draw the existing Open Folder and Run Build buttons.

- [ ] **Step 3: Register the panel**

In `src/app/layers/DebugLayer.cpp`, add the include beside the existing
`#include "debug/ProjectPanel.hpp"` at line 39:

```cpp
#include "debug/BuildPanel.hpp"
```

and register the panel in the `m_panels.push_back(...)` block around line 472:

```cpp
		m_panels.push_back(std::make_unique<BuildPanel>());
```

- [ ] **Step 4: Build and check it renders**

Run: `cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc && cmake --build D:\AetherCore\build\vs2022-msvc --target Editor --config Debug`
Then: `D:/AetherCore/build/vs2022-msvc/src/app/Debug/Editor.exe --project D:\AetherCore\projects\Whisper --no-validation`

Expected: a **Build** entry in the panel menu. Opening it shows the banner `Debug build - for testing, not for shipping`, the destination path, the action row, and an empty status area.

- [ ] **Step 5: Publish from the new panel**

Click Publish. Expected: an inline progress bar cycling through the nine stage names, then a success line and the Open Folder / Run Build buttons. Click Run Build; the Whisper title screen appears.

- [ ] **Step 6: Commit**

```bash
git add src/app/debug/BuildPanel.hpp src/app/debug/BuildPanel.cpp src/app/layers/DebugLayer.cpp
git commit -F - <<'EOF'
Add a Build panel for pack, shaders, scripts and publish

- Show the build configuration and destination before publishing
- Report publish progress inline instead of in a modal
- Collapse four parallel status strings into one, with remediation text
EOF
```

---

## Task 9: Shrink ProjectPanel to project identity

**Files:**
- Modify: `src/app/debug/ProjectPanel.hpp`, `src/app/debug/ProjectPanel.cpp`

- [ ] **Step 1: Delete the Build tab and the tab bar**

Remove the `ImGui::BeginTabBar("##projectTabs")` block. The panel becomes a flat layout: the toolbar row (Launcher / Reload / Open Root), the folder table with Repair, then the startup scene control.

Delete `DrawSceneTable` and `m_scenes` / `SceneEntry` entirely — the Hierarchy panel's scene list owns scene browsing.

- [ ] **Step 2: Replace the scene table with a combo**

```cpp
		ImGui::SeparatorText("Startup Scene");
		iw::LabelColumn("Boots");
		const std::string current = m_startupScene.empty() ? std::string("(none)") : m_startupScene;
		if (ImGui::BeginCombo("##startupScene", current.c_str()))
		{
			for (const std::string& name: m_sceneNames)
			{
				const bool selected = name == m_startupScene;
				if (ImGui::Selectable(name.c_str(), selected))
				{
					std::string error;
					if (app::WriteProjectStartupScene(project->projectFile, name, error))
					{
						m_startupScene = name;
						m_status = "Startup scene set to '" + name + "'.";
					}
					else
					{
						m_status = error;
					}
				}
				if (selected)
				{
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("The scene a published game boots. Also settable with the star in the Scenes list.");
```

`m_sceneNames` is a `std::vector<std::string>` replacing the old `m_scenes` vector of
`SceneEntry`. `Refresh` already walks the scenes directory (`ProjectPanel.cpp:265-289`);
keep that walk but collect names only:

```cpp
		m_sceneNames.clear();
		std::error_code ec;
		if (std::filesystem::is_directory(project.scenesDir, ec))
		{
			for (const auto& entry: std::filesystem::directory_iterator(project.scenesDir, ec))
			{
				if (ec || !entry.is_regular_file(ec))
				{
					continue;
				}
				const std::string name = SceneNameFromPath(entry.path());
				if (!name.empty())
				{
					m_sceneNames.push_back(name);
				}
			}
		}
		std::ranges::sort(m_sceneNames);
```

`SceneNameFromPath` already exists at `ProjectPanel.cpp:64` and returns an empty string for
files that are not `*.scene.toml`, so the filter above needs no extra extension check.

Because the combo writes immediately, delete `m_dirtySettings`, `SaveProjectSettings`, the
Save Project Settings button and the Clear Startup button.

- [ ] **Step 3: Build and check**

Run: `cmake --build D:\AetherCore\build\vs2022-msvc --target Editor --config Debug`
Then launch the editor on Whisper.

Expected: the Project panel is a single screen with no tabs; the startup-scene combo reads `Title`. Pick `Arena`, confirm `projects/Whisper/ProjectSettings.toml` now has `startupscene = "Arena"` and every other section is intact, then set it back to `Title`.

- [ ] **Step 4: Run the full suite**

Run: `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe`
Expected: `Status: SUCCESS!`, 649 test cases.

- [ ] **Step 5: Commit**

```bash
git add src/app/debug/ProjectPanel.hpp src/app/debug/ProjectPanel.cpp
git commit -F - <<'EOF'
Reduce the Project panel to project identity

- Drop the tab bar and the Build tab, now the Build panel's job
- Replace the scene table with a startup-scene combo; Hierarchy owns scene browsing
- Write the startup scene on selection instead of behind a dirty flag and Save button
EOF
```

---

## Task 10: Final verification and cleanup

**Files:** none created; verification only.

- [ ] **Step 1: Confirm the retired surface is gone**

Run: `grep -rn "usePackageTemplate\|verifyOutput\|syncEditorRuntimeProjectPak\|EditorProjectPublishOptions\|packageTemplateDir\|m_publishOutputRoot" src/ tests/`
Expected: no output.

- [ ] **Step 2: Clean-build everything from scratch**

Run:
```bash
cmake -S D:\AetherCore -B D:\AetherCore\build\vs2022-msvc
cmake --build D:\AetherCore\build\vs2022-msvc --target Editor GameRuntime Launcher EngineTests --config Debug
```
Expected: `0 Error(s)` for every target.

- [ ] **Step 3: Run the full suite three times**

Run `D:\AetherCore\build\vs2022-msvc\tests\Debug\EngineTests.exe` three times.
Expected: `Status: SUCCESS!` every run, 649 test cases. Three runs because this suite has shown intermittent behaviour after a test-TU-only rebuild.

- [ ] **Step 4: Full publish gauntlet on Whisper**

1. Launch the editor on Whisper, open the Build panel, publish.
2. Run `D:/AetherCore/projects/Whisper/Builds/Windows/Whisper/AetherGame.exe`; expect the title screen and `Startup scene 'Title' loaded (32 entities)`.
3. Open `publish-report.txt`; expect `Configuration: Debug  (testing only - not a shippable build)`.
4. Confirm `projects/Whisper/ProjectSettings.toml` has no `[publish]` section and still has `[app]`, `[graphics]`, `[paths]` and `[project]`.
5. Confirm the editor's own `build/vs2022-msvc/src/app/Debug/data/project.pak` was **not** touched by the publish (publish no longer syncs it; only Pack does).

- [ ] **Step 5: Negative test — publish with no startup scene**

Temporarily set `startupscene = ""` in Whisper's `ProjectSettings.toml`, reload the project, and publish.
Expected: publish fails at "Validating project" with a message naming the missing startup scene and a remediation line pointing at the Project panel. No package directory is created or emptied. Restore `startupscene = "Title"` afterwards.

- [ ] **Step 6: Revert incidental churn and commit**

Opening a project rewrites float formatting in touched `.scene.toml` files. Check `git status`, revert anything under `projects/` that the verification run touched, then:

```bash
git add -A src tests docs
git commit -m "Finish the publish and Project panel overhaul"
git push origin master
```

---

## Self-Review Notes

**Spec coverage:** every spec section maps to a task — publish TU split (Tasks 1–5), step list and computed progress (Task 4), staging from the editor bundle (Task 4 Step 2), deletions (Tasks 5–6), `[publish]` retirement (Task 7), panel split (Tasks 8–9), the new tests the spec asks for (Tasks 1–2), and the config banner from decision 4 (Tasks 1, 3, 8).

**One spec item deliberately changed:** the debug-CRT hard failure, documented at the top of this plan. Without that change, decision 4 is unimplementable.

**Two items the spec listed that this plan does not do:**
- The spec mentions `PublishFailure { step, message, remediation }`. This plan carries the same three fields on `StepResult` and `EditorProjectActionResult` rather than adding a fourth struct — the `step` name is already known at the point where a failure is turned into a result, so a dedicated type would only be passed once.
- Verification of "no shaders in engine.pak" is exercised only against a real pak, not a synthesised one; a stub `engine.pak` cannot be opened by `PakBackend`. Task 2's test acknowledges this rather than pretending to cover it.
