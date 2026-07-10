# In-Process Asset Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn AssetPacker into a static library the editor links and calls in-process, eliminating the `cmd.exe` shell-out that fails publish/pack with error 123, and harden the `.pak` format for shipped paks.

**Architecture:** Split `tools/assetpack` into an `AssetPipeline` static library (all logic, single source of truth) plus a thin `AssetPacker` CLI kept only for the CMake build-time bake. The editor (`App`) links `AssetPipeline` and calls a clean result-returning API in-process; the shipped `GameRuntime` does not link it. The one remaining shell-out (`dotnet build`) is routed through a single correctly-quoted process helper, and the `.pak` format gains a validated index (v2).

**Tech Stack:** C++20, CMake (Ninja + MSVC presets), doctest, zstd, freetype, cgltf, stb_image, bc7enc, xxHash.

**Reference spec:** `docs/superpowers/specs/2026-07-10-in-process-asset-pipeline-design.md`

---

## Conventions used in this plan

- **Build a target:** `cmake --build build-ninja-clang --target <Target>`
- **Run the test suite:** `ctest --test-dir build-ninja-clang -R EngineTests --output-on-failure`
- **Run one doctest case:** `build-ninja-clang/tests/EngineTests.exe --test-case="<name>"`
  (doctest exits 0 on pass, 1 on failure)
- **Reconfigure after CMake edits:** `cmake --preset vs2022-clang` is the IDE path;
  for the Ninja tree just run the build command — CMake auto-reconfigures because
  `CONFIGURE_DEPENDS` globs are in use.
- Every commit message ends with the required trailer:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

---

## File Structure

**New files:**
- `tools/assetpack/AssetPipeline.hpp` — public library API (`aether::assetpipeline`), clean includes only.
- `tools/assetpack/AssetPipeline.cpp` — API implementation over `PakWriter`/`MaterialImporter`.
- `tools/assetpack/StbImageImpl.cpp` — stb_image implementation TU, compiled only into the `AssetPacker` exe.
- `src/engine/io/Process.hpp` / `src/engine/io/Process.cpp` — `aether::io` cross-platform subprocess helper with correct per-platform quoting.
- `src/app/editor/EditorEnginePak.hpp` / `EditorEnginePak.cpp` — editor-side engine.pak baker.
- `tests/assetpack/AssetPipelineTests.cpp` — round-trip + API tests.
- `tests/io/ProcessTests.cpp` — command-wrapping unit tests.
- `tests/assetpack/PakFormatTests.cpp` — format-hardening tests.

**Modified files:**
- `tools/assetpack/CMakeLists.txt` — split into `AssetPipeline` STATIC lib + thin `AssetPacker` exe.
- `tools/assetpack/ThirdPartyImpl.cpp` — drop `STB_IMAGE_IMPLEMENTATION` (moves to `StbImageImpl.cpp`).
- `tools/assetpack/*.hpp` / `*.cpp` — wrap in `namespace aether::assetpipeline`.
- `tools/assetpack/main.cpp` — thin CLI over the library API.
- `src/app/CMakeLists.txt` — link `AssetPipeline` into `App`; add `AETHER_ENGINE_RESOURCES_DIR`.
- `src/app/editor/EditorProjectPublisher.cpp` / `.hpp` — in-process pack; delete shell-out/exe-finder; engine.pak bake in publish.
- `src/app/editor/EditorProjectActions.hpp` — add `rebuildEnginePak` action.
- `src/app/debug/EditorProjectManager.cpp` — wire `rebuildEnginePak`.
- `src/app/debug/ProjectPanel.cpp` — "Rebuild Engine Pak" button.
- `src/app/scripting/CSharpScriptingSubsystem.cpp` — route `dotnet build` through `aether::io::RunProcess`.
- `include/PakFormat.hpp` — 64-byte header with `indexHash`; bump `PAK_VERSION` to 2.
- `tools/assetpack/PakWriter.cpp` — compute + write `indexHash`.
- `src/engine/io/PakBackend.cpp` — validate header bounds + `indexHash`.
- `tests/CMakeLists.txt` — add new test sources; link `AssetPipeline`.

---

## Task 1: Split into `AssetPipeline` library + thin `AssetPacker` exe (stb de-dup)

Pure build-system refactor. Move all packer logic into a static library; keep the
exe as a thin frontend. Move the stb_image implementation into a TU compiled only
by the exe so the library carries no stb definition (it will link against the
engine's stb when linked into `App` later).

**Files:**
- Create: `tools/assetpack/StbImageImpl.cpp`
- Modify: `tools/assetpack/ThirdPartyImpl.cpp`
- Modify: `tools/assetpack/CMakeLists.txt`

- [ ] **Step 1: Create the exe-only stb implementation TU**

Create `tools/assetpack/StbImageImpl.cpp`:

```cpp
// stb_image implementation, compiled ONCE into the AssetPacker executable only.
// The AssetPipeline library deliberately does NOT define this: when the library
// is linked into the editor (App), the engine already provides stb_image's
// implementation (src/engine/material/Texture.cpp), and a second definition
// would be a duplicate-symbol link error.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
```

- [ ] **Step 2: Remove the stb implementation from the shared third-party TU**

In `tools/assetpack/ThirdPartyImpl.cpp`, delete these two lines (keep cgltf and
xxhash — the engine does not define those):

```cpp
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
```

Resulting `ThirdPartyImpl.cpp`:

```cpp
// ---------------------------------------------------------------------------
// Single-header library implementations compiled once into the AssetPipeline
// library. stb_image is NOT here: it lives in StbImageImpl.cpp (exe-only) so the
// library can be linked into the engine, which already defines stb_image.
// ---------------------------------------------------------------------------

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include <xxhash.h>
```

- [ ] **Step 3: Rewrite `tools/assetpack/CMakeLists.txt` as library + exe**

Replace the entire file with:

```cmake
# All packer logic lives in the AssetPipeline static library (single source of
# truth for pak production). The AssetPacker executable is a thin CLI frontend
# kept for the CMake build-time bake and CI/headless use. The editor (App) links
# AssetPipeline directly and packs in-process.

file(GLOB_RECURSE ASSETPIPELINE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE ASSETPIPELINE_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")

# main.cpp and the exe-only stb impl are NOT part of the library.
list(REMOVE_ITEM ASSETPIPELINE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StbImageImpl.cpp"
)

add_library(AssetPipeline STATIC
    ${ASSETPIPELINE_SOURCES}
    ${ASSETPIPELINE_HEADERS}
    "${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp"
    "${bc7enc_rdo_SOURCE_DIR}/rgbcx.cpp"
)

# Consumers (exe, editor, tests) include AssetPipeline.hpp from this dir.
target_include_directories(AssetPipeline PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(AssetPipeline PRIVATE
    "${CMAKE_SOURCE_DIR}/include"   # PakFormat.hpp, AeBnFormat.hpp
    "${xxHash_SOURCE_DIR}"
)

# External libraries use deprecated CRT functions / have unused internals on
# Windows; mark as SYSTEM to suppress warnings.
target_include_directories(AssetPipeline SYSTEM PRIVATE
    "${bc7enc_rdo_SOURCE_DIR}"      # bc7enc.h, rgbcx.h
    "${stb_SOURCE_DIR}"             # stb_image.h
    "${cgltf_SOURCE_DIR}"           # cgltf.h
    "${glm_SOURCE_DIR}"             # glm/glm.hpp
)

target_link_libraries(AssetPipeline PRIVATE zstd::libzstd_static freetype)

if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(AssetPipeline PRIVATE tomlplusplus::tomlplusplus)
elseif(TARGET tomlplusplus_tomlplusplus)
    target_link_libraries(AssetPipeline PRIVATE tomlplusplus_tomlplusplus)
else()
    target_include_directories(AssetPipeline SYSTEM PRIVATE "${tomlplusplus_SOURCE_DIR}/include")
endif()

# Thin CLI frontend. Compiles the exe-only stb_image implementation and links the
# library. stb include dir is needed for StbImageImpl.cpp.
add_executable(AssetPacker
    main.cpp
    StbImageImpl.cpp
)
target_link_libraries(AssetPacker PRIVATE AssetPipeline)
target_include_directories(AssetPacker SYSTEM PRIVATE "${stb_SOURCE_DIR}")

set_target_properties(AssetPacker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools"
)

aethercore_enable_dead_strip_report(AssetPacker)
```

- [ ] **Step 4: Build the exe and verify it still links**

Run: `cmake --build build-ninja-clang --target AssetPacker`
Expected: builds `AssetPipeline` then `AssetPacker` with no duplicate-symbol errors, exe at `build-ninja-clang/tools/AssetPacker.exe`.

- [ ] **Step 5: Smoke-test the CLI still packs**

Run:
```bash
mkdir -p /tmp/aepak_fixture && printf 'hello' > /tmp/aepak_fixture/a.txt
build-ninja-clang/tools/AssetPacker.exe /tmp/aepak_fixture /tmp/aepak_out.pak
```
Expected: console shows `AssetPacker: done.` with 1 entry (plus manifest), and `/tmp/aepak_out.pak` exists.

- [ ] **Step 6: Commit**

```bash
git add tools/assetpack/CMakeLists.txt tools/assetpack/StbImageImpl.cpp tools/assetpack/ThirdPartyImpl.cpp
git commit -m "$(printf 'Split AssetPacker into AssetPipeline library + thin CLI\n\nMove stb_image impl into an exe-only TU so the library carries no stb\ndefinition and can later link into the engine.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Task 2: Namespace the pipeline under `aether::assetpipeline`

Wrap every packer header and source in `namespace aether::assetpipeline { ... }`,
moving the file-scope `namespace fs = std::filesystem;` aliases inside the
namespace so they no longer leak at global scope. Pure mechanical change; the
classes already reference each other unqualified, so wrapping them all in the
same namespace keeps those references valid.

**Files (wrap contents of each in the namespace):**
- Modify: `tools/assetpack/PakWriter.hpp`, `PakWriter.cpp`
- Modify: `tools/assetpack/AssetProcessor.hpp`, `PipelineUtils.hpp`, `PakLog.hpp`, `PakManifest.hpp`
- Modify: `tools/assetpack/TextureProcessor.hpp`, `TextureProcessor.cpp`
- Modify: `tools/assetpack/MeshProcessor.hpp`, `MeshProcessor.cpp`
- Modify: `tools/assetpack/MaterialProcessor.hpp`, `MaterialProcessor.cpp`
- Modify: `tools/assetpack/MaterialImporter.hpp`, `MaterialImporter.cpp`
- Modify: `tools/assetpack/SpirvProcessor.hpp`, `SpirvProcessor.cpp`
- Modify: `tools/assetpack/FontProcessor.hpp`, `FontProcessor.cpp`
- Modify: `tools/assetpack/ThreadPool.hpp`, `DDSFormat.hpp`
- Modify: `tools/assetpack/main.cpp`

- [ ] **Step 1: Wrap each file's declarations in the namespace**

For every header/source listed above, apply this exact transformation: after the
`#include` block (and any file-scope `#define`/`#pragma pack`), open the namespace;
close it at end of file. Move any `namespace fs = std::filesystem;` line to just
inside the opened namespace. Example for `PakWriter.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "PakFormat.hpp"

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	// ... existing PakWriter class unchanged ...
}
```

Example for a `.cpp` (e.g. `PakWriter.cpp`): keep all `#include`s and the
`#include <PakFormat.hpp>` at the top outside the namespace, then:

```cpp
namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	// ... existing anonymous namespace + PakWriter methods unchanged ...
}
```

Note: `FontProcessor.hpp` defines on-disk structs the engine's font loader may
include. Keep `#pragma pack(push,1)` INSIDE the namespace with the structs; the
runtime font loader (if it includes this header) must reference
`aether::assetpipeline::FontProcessor::FontAtlasHeader`. Grep first:
`grep -rn "FontProcessor::" src/` — if the engine references these types, update
those references in this step.

- [ ] **Step 2: Update `main.cpp` to qualify the calls**

At the top of `main.cpp`, after includes, add:

```cpp
using namespace aether::assetpipeline;
```

(`ParseArgs`/`main` then reference `PakWriter`, `MaterialImporter`,
`FontProcessor` unchanged.)

- [ ] **Step 3: Build the exe**

Run: `cmake --build build-ninja-clang --target AssetPacker`
Expected: compiles cleanly; the compiler flags any missed reference so nothing is silently wrong.

- [ ] **Step 4: Smoke-test parity**

Run:
```bash
build-ninja-clang/tools/AssetPacker.exe /tmp/aepak_fixture /tmp/aepak_out2.pak
```
Expected: identical `AssetPacker: done.` output as Task 1 Step 5.

- [ ] **Step 5: Commit**

```bash
git add tools/assetpack
git commit -m "$(printf 'Namespace asset pipeline under aether::assetpipeline\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Task 3: Public library API + round-trip test (TDD)

Add the editor-facing API and a doctest round-trip that packs a temp directory
via the library and reads it back through the engine's `PakBackend`.

**Files:**
- Create: `tools/assetpack/AssetPipeline.hpp`
- Create: `tools/assetpack/AssetPipeline.cpp`
- Create: `tests/assetpack/AssetPipelineTests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the public API header**

Create `tools/assetpack/AssetPipeline.hpp`:

```cpp
#pragma once

// Public entry point for the asset pipeline. Callers (the editor, the CLI, tests)
// include ONLY this header — it exposes no third-party or internal packer types,
// so linking AssetPipeline into the engine stays clean.

#include <cstdint>
#include <filesystem>
#include <string>

namespace aether::assetpipeline
{
	struct PackOptions
	{
		int  compressionLevel = 3;
		bool importMaterials  = false; // run MaterialImporter before packing
		bool projectLayout    = false; // require ProjectSettings.toml; import from assets/ subdir
	};

	struct PackResult
	{
		bool                  ok = false;
		std::string           message;     // human-readable summary or failure reason
		std::uint64_t         sourceFiles = 0;
		std::uintmax_t        pakBytes = 0;
		std::filesystem::path outputPath;
	};

	// Pack a project root: optional material import + project descriptor check.
	[[nodiscard]] PackResult PackProject(const std::filesystem::path& projectRoot,
	                                      const std::filesystem::path& outputPak,
	                                      const PackOptions& options);

	// Pack an arbitrary asset directory (used for engine.pak).
	[[nodiscard]] PackResult PackDirectory(const std::filesystem::path& sourceDir,
	                                        const std::filesystem::path& outputPak,
	                                        const PackOptions& options);
}
```

- [ ] **Step 2: Write the API implementation**

Create `tools/assetpack/AssetPipeline.cpp`:

```cpp
#include "AssetPipeline.hpp"

#include <system_error>

#include "MaterialImporter.hpp"
#include "PakWriter.hpp"

namespace aether::assetpipeline
{
	namespace
	{
		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			std::error_code ec;
			return std::filesystem::is_regular_file(root / "ProjectSettings.toml", ec);
		}

		PackResult RunPack(const std::filesystem::path& sourceDir, const std::filesystem::path& outputPak, const PackOptions& options)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(sourceDir, ec))
			{
				return {.ok = false, .message = "Source directory not found: " + sourceDir.generic_string(), .outputPath = outputPak};
			}

			if (options.importMaterials)
			{
				const std::filesystem::path materialRoot =
				        (options.projectLayout && std::filesystem::is_directory(sourceDir / "assets", ec)) ? sourceDir / "assets" : sourceDir;
				if (MaterialImporter::ImportDirectory(materialRoot) < 0)
				{
					return {.ok = false, .message = "Material import failed for: " + materialRoot.generic_string(), .outputPath = outputPak};
				}
			}

			PakWriter writer(options.compressionLevel);
			writer.AddDirectory(sourceDir);
			const std::uint64_t sourceFiles = static_cast<std::uint64_t>(writer.FileCount());

			if (!writer.Write(outputPak))
			{
				return {.ok = false,
				        .message = "Pack failed; see " + std::filesystem::path(outputPak.string() + ".log").generic_string(),
				        .sourceFiles = sourceFiles,
				        .outputPath = outputPak};
			}

			std::uintmax_t bytes = std::filesystem::file_size(outputPak, ec);
			if (ec)
			{
				bytes = 0;
			}
			return {.ok = true,
			        .message = "Packed " + std::to_string(sourceFiles) + " file(s) (" + std::to_string(bytes / 1024) + " KB).",
			        .sourceFiles = sourceFiles,
			        .pakBytes = bytes,
			        .outputPath = outputPak};
		}
	} // namespace

	PackResult PackProject(const std::filesystem::path& projectRoot, const std::filesystem::path& outputPak, const PackOptions& options)
	{
		if (options.projectLayout && !HasProjectDescriptor(projectRoot))
		{
			return {.ok = false, .message = "Project descriptor is missing: " + (projectRoot / "ProjectSettings.toml").generic_string(), .outputPath = outputPak};
		}
		return RunPack(projectRoot, outputPak, options);
	}

	PackResult PackDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& outputPak, const PackOptions& options)
	{
		return RunPack(sourceDir, outputPak, options);
	}
}
```

- [ ] **Step 3: Write the failing round-trip test**

Create `tests/assetpack/AssetPipelineTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "AssetPipeline.hpp"
#include "io/PakBackend.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeTempDir(const std::string& tag)
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("aepak_test_" + tag);
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		return dir;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& contents)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		out << contents;
	}
}

TEST_CASE("PackDirectory round-trips plain files through PakBackend")
{
	const std::filesystem::path src = MakeTempDir("roundtrip_src");
	WriteFile(src / "hello.txt", "hello world");
	WriteFile(src / "nested" / "data.bytes", std::string(4096, 'x'));

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_roundtrip.pak";
	std::filesystem::remove(pak);

	const assetpipeline::PackResult result = assetpipeline::PackDirectory(src, pak, {});
	REQUIRE(result.ok);
	CHECK(std::filesystem::exists(pak));

	io::PakBackend backend(pak);
	CHECK(backend.Exists("hello.txt"));
	CHECK(backend.Exists("nested/data.bytes"));

	const auto bytes = backend.Read("hello.txt");
	REQUIRE(bytes.has_value());
	const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
	CHECK(text == "hello world");
}

TEST_CASE("PackProject fails cleanly when the descriptor is missing")
{
	const std::filesystem::path src = MakeTempDir("noproj");
	WriteFile(src / "a.txt", "a");

	const std::filesystem::path pak = std::filesystem::temp_directory_path() / "aepak_test_noproj.pak";
	const assetpipeline::PackResult result = assetpipeline::PackProject(src, pak, {.projectLayout = true});
	CHECK_FALSE(result.ok);
	CHECK(result.message.find("descriptor is missing") != std::string::npos);
}
```

- [ ] **Step 4: Wire the tests into CMake**

In `tests/CMakeLists.txt`, add to the `add_executable(EngineTests ...)` source list
(after the `imgui/ImguiFrameDataTests.cpp` line):

```cmake
    assetpack/AssetPipelineTests.cpp
```

And change the link line from:

```cmake
target_link_libraries(EngineTests PRIVATE Engine doctest::doctest)
```

to:

```cmake
target_link_libraries(EngineTests PRIVATE Engine AssetPipeline doctest::doctest)
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cmake --build build-ninja-clang --target EngineTests && build-ninja-clang/tests/EngineTests.exe --test-case="PackDirectory round-trips plain files through PakBackend"`
Expected: PASS. Then run the descriptor case:
`build-ninja-clang/tests/EngineTests.exe --test-case="PackProject fails cleanly when the descriptor is missing"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add tools/assetpack/AssetPipeline.hpp tools/assetpack/AssetPipeline.cpp tests/assetpack/AssetPipelineTests.cpp tests/CMakeLists.txt
git commit -m "$(printf 'Add AssetPipeline public API with round-trip tests\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Task 4: Editor packs in-process (fixes the bug)

Replace the `std::system` pack shell-out in the publisher with a direct library
call, and delete the now-dead exe-finding/quoting code.

**Files:**
- Modify: `src/app/CMakeLists.txt`
- Modify: `src/app/editor/EditorProjectPublisher.hpp`
- Modify: `src/app/editor/EditorProjectPublisher.cpp`

- [ ] **Step 1: Link the library into the editor only**

In `src/app/CMakeLists.txt`, change:

```cmake
target_link_libraries(App PRIVATE Engine)
```

to:

```cmake
target_link_libraries(App PRIVATE Engine AssetPipeline)
```

(Leave the `GameRuntime` link line untouched — the runtime must not link the pipeline.)

- [ ] **Step 2: Remove the dead config field from the header**

In `src/app/editor/EditorProjectPublisher.hpp`, delete the line:

```cpp
		std::filesystem::path assetPackerExe;
```

- [ ] **Step 3: Replace the pack shell-out with a library call**

In `src/app/editor/EditorProjectPublisher.cpp`:

Add near the top with the other includes:

```cpp
#include "AssetPipeline.hpp"
```

Replace the body of `PackProjectInternal` (the block from
`if (config.assetPackerExe.empty())` through the `std::system(command.c_str())`
error handling, i.e. current lines ~574–599) so it becomes:

```cpp
		EditorProjectActionResult PackProjectInternal(const EditorProjectContext& project, const EditorProjectPublishConfig& config, bool syncEditorRuntimeProjectPak)
		{
			if (std::optional<EditorProjectActionResult> validationError = ValidateProjectForPackaging(project))
			{
				return *validationError;
			}

			const std::filesystem::path exeDataDir = config.executableDir / "data";
			const std::filesystem::path outputDir = project.root / "Builds" / "Pack";
			if (auto dirResult = io::file_util::CreateDirectories(outputDir); !dirResult)
			{
				return {.succeeded = false, .message = "Could not create output data directory: " + dirResult.error().message};
			}

			const std::filesystem::path outputPak = outputDir / "project.pak";
			const assetpipeline::PackResult packResult =
			        assetpipeline::PackProject(project.root, outputPak, {.importMaterials = true, .projectLayout = true});
			if (!packResult.ok)
			{
				return {.succeeded = false, .message = packResult.message, .outputPath = outputPak};
			}

			// ... existing syncEditorRuntimeProjectPak copy block stays unchanged ...
```

Keep everything from the `if (syncEditorRuntimeProjectPak && outputDir != exeDataDir)`
block downward exactly as-is (the size report, `AE_INFO`, and success return).

- [ ] **Step 4: Delete the now-dead helpers and config wiring**

In `src/app/editor/EditorProjectPublisher.cpp`, delete:
- the `ShellQuotePath` function (lines ~72–89) — verify no other caller with
  `grep -n "ShellQuotePath" src/app/editor/EditorProjectPublisher.cpp`; it is
  used by the dotnet build in `RunCommandToLog` (Task 5 replaces that). **Do this
  deletion in Task 5, not here** — leave `ShellQuotePath` for now.
- `FindAssetPackerExecutable` function (lines ~410–437).
- In `MakeDefaultEditorProjectPublishConfig`, delete the block:

```cpp
	if (const std::optional<std::filesystem::path> packer = FindAssetPackerExecutable(config.executableDir))
	{
		config.assetPackerExe = *packer;
	}
```

Verify no remaining references: `grep -n "assetPackerExe\|FindAssetPackerExecutable" src/app`
Expected: no matches.

- [ ] **Step 5: Build the editor**

Run: `cmake --build build-ninja-clang --target App`
Expected: links cleanly (no duplicate stb symbols — the library provides none).

- [ ] **Step 6: Verify the pack path no longer shells out**

Run: `grep -n "std::system" src/app/editor/EditorProjectPublisher.cpp`
Expected: only the `dotnet build` occurrence inside `RunCommandToLog` remains (removed in Task 5); the pack `std::system` call is gone.

- [ ] **Step 7: Commit**

```bash
git add src/app/CMakeLists.txt src/app/editor/EditorProjectPublisher.hpp src/app/editor/EditorProjectPublisher.cpp
git commit -m "$(printf 'Pack projects in-process from the editor\n\nReplace the AssetPacker.exe shell-out (source of the Windows error-123\nquote-stripping bug) with a direct AssetPipeline call. Delete the exe\nfinder and its config field.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Task 5: Single process helper for the `dotnet build` shell-out (§6)

Introduce one correctly-quoted subprocess helper in `aether::io` and route both
remaining `dotnet build` callers through it, deleting the duplicated (and, in the
publisher, buggy) quoting logic. The command-wrapping is a pure function, so it
gets a real unit test.

**Files:**
- Create: `src/engine/io/Process.hpp`, `src/engine/io/Process.cpp`
- Create: `tests/io/ProcessTests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/app/editor/EditorProjectPublisher.cpp`
- Modify: `src/app/scripting/CSharpScriptingSubsystem.cpp`

- [ ] **Step 1: Write the helper header**

Create `src/engine/io/Process.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <string>

namespace aether::io
{
	// Wraps a shell command so it survives cmd.exe's /c quote handling on Windows.
	// On Windows, cmd strips the first and last quote of the command line; a command
	// that itself starts and ends with quoted paths therefore loses them. Wrapping
	// the whole command in an extra outer quote pair makes the strip a no-op. On
	// POSIX shells the command is returned unchanged.
	[[nodiscard]] std::string WrapShellCommand(const std::string& command);

	// Runs `command` through the platform shell, redirecting combined stdout+stderr
	// to `logPath`. Returns the process exit code (0 == success), or -1 if the shell
	// could not be started. Creates the log's parent directory if needed.
	[[nodiscard]] int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath);

	// Runs `command` through the platform shell, capturing combined stdout+stderr
	// into `output`. Returns the process exit code, or -1 on failure to start.
	[[nodiscard]] int RunProcessCapture(const std::string& command, std::string& output);
}
```

- [ ] **Step 2: Write the failing unit test for the wrapper**

Create `tests/io/ProcessTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include "io/Process.hpp"

using namespace aether;

TEST_CASE("WrapShellCommand makes cmd.exe quote-stripping a no-op on Windows")
{
	const std::string inner = "\"C:\\tools\\AssetPacker.exe\" --project \"C:\\my project\" \"C:\\out.pak\"";
	const std::string wrapped = io::WrapShellCommand(inner);
#ifdef _WIN32
	// Outer quote pair added so cmd's first/last-quote strip leaves `inner` intact.
	CHECK(wrapped.front() == '"');
	CHECK(wrapped.back() == '"');
	CHECK(wrapped == "\"" + inner + "\"");
#else
	CHECK(wrapped == inner);
#endif
}
```

- [ ] **Step 3: Run the test to verify it fails to link**

Run: `cmake --build build-ninja-clang --target EngineTests`
Expected: FAIL — unresolved `aether::io::WrapShellCommand` (implementation not written yet).

- [ ] **Step 4: Write the helper implementation**

Create `src/engine/io/Process.cpp`:

```cpp
#include "io/Process.hpp"

#include <array>
#include <cstdio>
#include <system_error>

namespace aether::io
{
	std::string WrapShellCommand(const std::string& command)
	{
#ifdef _WIN32
		// cmd.exe /c strips the first and last quote of the command line. Wrapping
		// the whole thing in an extra pair makes that strip restore the original.
		return "\"" + command + "\"";
#else
		return command;
#endif
	}

	int RunProcessToLog(const std::string& command, const std::filesystem::path& logPath)
	{
		std::error_code ec;
		std::filesystem::create_directories(logPath.parent_path(), ec);

		std::string redirect = command + " > \"" + logPath.string() + "\" 2>&1";
		const std::string wrapped = WrapShellCommand(redirect);
		const int rc = std::system(wrapped.c_str());
		return rc;
	}

	int RunProcessCapture(const std::string& command, std::string& output)
	{
		const std::string wrapped = WrapShellCommand(command);
#ifdef _WIN32
		FILE* pipe = _popen(wrapped.c_str(), "r");
#else
		FILE* pipe = popen(wrapped.c_str(), "r");
#endif
		if (pipe == nullptr)
		{
			return -1;
		}
		std::array<char, 512> buffer{};
		while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
		{
			output += buffer.data();
		}
#ifdef _WIN32
		return _pclose(pipe);
#else
		return pclose(pipe);
#endif
	}
}
```

Note: `src/engine/CMakeLists.txt` globs its sources with `CONFIGURE_DEPENDS`
(verify with `grep -n "GLOB" src/engine/CMakeLists.txt`); the new `Process.cpp`
is picked up automatically. If the engine lists sources explicitly instead, add
`io/Process.cpp` and `io/Process.hpp` to that list.

- [ ] **Step 5: Add the test source to CMake and run it**

In `tests/CMakeLists.txt`, add to the `EngineTests` source list:

```cmake
    io/ProcessTests.cpp
```

Run: `cmake --build build-ninja-clang --target EngineTests && build-ninja-clang/tests/EngineTests.exe --test-case="WrapShellCommand makes cmd.exe quote-stripping a no-op on Windows"`
Expected: PASS.

- [ ] **Step 6: Route the publisher's dotnet build through the helper**

In `src/app/editor/EditorProjectPublisher.cpp`:

Add include:

```cpp
#include "io/Process.hpp"
```

Delete the `RunCommandToLog` helper (lines ~109–129) and the `ShellQuotePath`
helper (lines ~72–89) — both are now dead once the call site changes. Replace the
dotnet build call site (currently `if (!RunCommandToLog(command, publishLog, error))`)
with:

```cpp
			const std::string command = "\"" + config.dotnetExe.string() + "\" build \"" + scriptsProject.string()
			        + "\" -c " + config.managedConfig + " --nologo -v:m -p:ArtifactsPath=\"" + artifactsDir.string() + "\"";
			if (const int rc = io::RunProcessToLog(command, publishLog); rc != 0)
			{
				const std::string excerpt = ReadLogExcerpt(publishLog);
				return {.succeeded = false,
				        .message = "Project script build failed (exit " + std::to_string(rc) + "). " + excerpt,
				        .outputPath = publishDir};
			}
```

Verify `ShellQuotePath` has no remaining callers:
`grep -n "ShellQuotePath" src/app/editor/EditorProjectPublisher.cpp` → no matches.

- [ ] **Step 7: Route the C# hot-reload build through the helper**

In `src/app/scripting/CSharpScriptingSubsystem.cpp`:

Add include `#include "io/Process.hpp"`. Delete the local `RunCapture` function
(lines ~80–102) and its `#ifdef _WIN32`/`_popen` body. Replace the block that
builds `command` and calls `RunCapture` (lines ~241–250) with:

```cpp
		const std::string inner = std::string("\"") + AETHER_DOTNET_EXE + "\" build \"" + gameProject.string() + "\" -c " + AETHER_MANAGED_CONFIG
		        + " --nologo -v:m -p:ArtifactsPath=\"" + AETHER_MANAGED_ARTIFACTS + "\" 2>&1";

		std::string output;
		const int rc = io::RunProcessCapture(inner, output);
```

(The manual `#ifdef _WIN32` re-wrap that was here is now handled inside
`RunProcessCapture` via `WrapShellCommand`.)

- [ ] **Step 8: Build and verify no shell-out duplication remains**

Run: `cmake --build build-ninja-clang --target App`
Expected: builds cleanly.
Run: `grep -rn "_popen\|std::system" src/app | grep -v Process.cpp`
Expected: no matches in `src/app` (both shell-outs now go through `aether::io`).

- [ ] **Step 9: Commit**

```bash
git add src/engine/io/Process.hpp src/engine/io/Process.cpp tests/io/ProcessTests.cpp tests/CMakeLists.txt src/app/editor/EditorProjectPublisher.cpp src/app/scripting/CSharpScriptingSubsystem.cpp
git commit -m "$(printf 'Consolidate dotnet shell-out into aether::io::RunProcess\n\nOne correctly-quoted process helper replaces two ad-hoc call sites,\nfixing the latent cmd.exe quote bug in the publisher path.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Task 6: Editor bakes engine.pak (§5)

Give the editor an in-process engine.pak baker (dev-gated), auto-baked during
Publish with a copy fallback, plus a Tools ▸ Rebuild Engine Pak action.

**Files:**
- Modify: `src/app/CMakeLists.txt`
- Create: `src/app/editor/EditorEnginePak.hpp`, `src/app/editor/EditorEnginePak.cpp`
- Modify: `src/app/editor/EditorProjectActions.hpp`
- Modify: `src/app/editor/EditorProjectPublisher.cpp`
- Modify: `src/app/debug/EditorProjectManager.cpp`
- Modify: `src/app/debug/ProjectPanel.cpp`

- [ ] **Step 1: Add the dev-only resources define**

In `src/app/CMakeLists.txt`, inside the existing
`target_compile_definitions(App PRIVATE ...)` block (the one with
`AETHERCORE_EDITOR_APP=1`), add:

```cmake
    AETHER_ENGINE_RESOURCES_DIR="${CMAKE_SOURCE_DIR}/resources"
```

(Only `App` gets it; `GameRuntime` and packaged builds leave it undefined.)

- [ ] **Step 2: Write the engine.pak baker**

Create `src/app/editor/EditorEnginePak.hpp`:

```cpp
#pragma once

#include <filesystem>
#include <optional>

#include "editor/EditorProjectActions.hpp"

namespace aether::app
{
	// True in dev checkouts (AETHER_ENGINE_RESOURCES_DIR defined); false in a
	// shipped editor with no engine resources tree.
	[[nodiscard]] bool CanBakeEnginePak();

	// Absolute path to the engine resources source dir, or nullopt when unavailable.
	[[nodiscard]] std::optional<std::filesystem::path> EngineResourcesDir();

	// Bakes engine-owned runtime resources into `outputEnginePak` (staging the
	// engine asset subdirs so virtual paths match the CMake build, e.g. "fonts/..").
	[[nodiscard]] EditorProjectActionResult BakeEnginePak(const std::filesystem::path& outputEnginePak);
}
```

Create `src/app/editor/EditorEnginePak.cpp`:

```cpp
#include "editor/EditorEnginePak.hpp"

#include <string_view>

#include "AssetPipeline.hpp"
#include "io/FileUtil.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::app
{
	namespace
	{
		// Engine-owned resource subdirs that go into engine.pak. Mirrors the CMake
		// POST_BUILD step (aethercore_add_runtime_payload in src/app/CMakeLists.txt),
		// which stages resources/fonts and packs it.
		constexpr std::string_view kEngineAssetSubdirs[] = {"fonts"};
	} // namespace

	bool CanBakeEnginePak()
	{
		return EngineResourcesDir().has_value();
	}

	std::optional<std::filesystem::path> EngineResourcesDir()
	{
#ifdef AETHER_ENGINE_RESOURCES_DIR
		std::error_code ec;
		const std::filesystem::path dir = AETHER_ENGINE_RESOURCES_DIR;
		if (std::filesystem::is_directory(dir, ec))
		{
			return dir;
		}
#endif
		return std::nullopt;
	}

	EditorProjectActionResult BakeEnginePak(const std::filesystem::path& outputEnginePak)
	{
		const std::optional<std::filesystem::path> resources = EngineResourcesDir();
		if (!resources)
		{
			return {.succeeded = false, .message = "Engine resources are not available in this build; cannot bake engine.pak."};
		}

		// Stage the engine-owned subdirs into a temp dir so packed virtual paths are
		// prefixed correctly (e.g. "fonts/Roboto.ttf").
		std::error_code ec;
		const std::filesystem::path staging = std::filesystem::temp_directory_path() / "aether_engine_pak_stage";
		std::filesystem::remove_all(staging, ec);
		if (auto dirResult = io::file_util::CreateDirectories(staging); !dirResult)
		{
			return {.succeeded = false, .message = "Could not create engine.pak staging dir: " + dirResult.error().message};
		}

		for (const std::string_view subdir: kEngineAssetSubdirs)
		{
			const std::filesystem::path from = *resources / subdir;
			if (!std::filesystem::is_directory(from, ec))
			{
				continue;
			}
			std::filesystem::copy(from, staging / subdir, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
			if (ec)
			{
				return {.succeeded = false, .message = "Could not stage engine assets '" + std::string(subdir) + "': " + ec.message()};
			}
		}

		const assetpipeline::PackResult result = assetpipeline::PackDirectory(staging, outputEnginePak, {});
		std::filesystem::remove_all(staging, ec);

		if (!result.ok)
		{
			return {.succeeded = false, .message = result.message, .outputPath = outputEnginePak};
		}
		AE_INFO(LogCategory::App, "Baked engine.pak to {}", outputEnginePak.generic_string());
		return {.succeeded = true, .message = "Baked engine.pak (" + std::to_string(result.pakBytes / 1024) + " KB).", .outputPath = outputEnginePak};
	}
}
```

- [ ] **Step 3: Use a fresh engine.pak during Publish when available**

In `src/app/editor/EditorProjectPublisher.cpp`, add include `#include "editor/EditorEnginePak.hpp"`.
Find the `CopyShippedDataPayload` call inside `PublishProject` (the
`else` branch that runs when not using the package template, currently
`if (!CopyShippedDataPayload(exeDataDir, publishDir / "data", error))`). Directly
**after** the shipped-data copy succeeds (both template and non-template paths
converge before `CopyIfExists(packResult.outputPath, ...)`), insert:

```cpp
		// Prefer a freshly baked engine.pak over the (possibly stale) build-tree copy
		// when this dev editor can bake one. A shipped editor keeps the copied pak.
		if (CanBakeEnginePak())
		{
			const EditorProjectActionResult bake = BakeEnginePak(publishDir / "data" / "engine.pak");
			if (!bake.succeeded)
			{
				return {.succeeded = false, .message = "Could not bake engine.pak: " + bake.message, .outputPath = publishDir};
			}
		}
```

(Place it after the `usePackageTemplate`/`else` block closes and before the
`CopyIfExists(packResult.outputPath, publishDir / "data" / "project.pak", ...)`
line, so it overwrites whatever engine.pak the copy produced.)

- [ ] **Step 4: Add the `rebuildEnginePak` action**

In `src/app/editor/EditorProjectActions.hpp`, add to the `EditorProjectActions`
struct:

```cpp
		std::function<EditorProjectActionResult()> rebuildEnginePak;
```

- [ ] **Step 5: Wire the action in the manager**

In `src/app/debug/EditorProjectManager.cpp`, add include
`#include "editor/EditorEnginePak.hpp"`. Next to the existing
`m_actions.packProject = ...` assignment (around line 381), add:

```cpp
		if (CanBakeEnginePak())
		{
			m_actions.rebuildEnginePak = []
			{
				const EditorProjectPublishConfig config = MakeDefaultEditorProjectPublishConfig();
				return BakeEnginePak(config.executableDir / "data" / "engine.pak");
			};
		}
```

- [ ] **Step 6: Add the Tools button in the project panel**

In `src/app/debug/ProjectPanel.cpp`, in the block that renders the
`ICON_FA_BOX_OPEN "  Pack Project"` button (around line 581), after that button's
`ImGui::EndDisabled();` and its `ImGui::SameLine();`, add:

```cpp
			if (actions->rebuildEnginePak)
			{
				if (ImGui::Button(ICON_FA_GEAR "  Rebuild Engine Pak"))
				{
					const EditorProjectActionResult result = actions->rebuildEnginePak();
					m_packSucceeded = result.succeeded;
					m_packStatus = result.message;
				}
				ImGui::SameLine();
			}
```

(If `ICON_FA_GEAR` is not defined in the icon font header used here, use
`ICON_FA_BOX_OPEN` to match the neighbouring button — grep the file's includes
for the `ICON_FA_` prefix in use.)

- [ ] **Step 7: Register the new sources (if not globbed) and build**

`src/app/CMakeLists.txt` globs `*.cpp`/`*.hpp` with `CONFIGURE_DEPENDS`, so
`EditorEnginePak.*` are picked up automatically for `App` (and excluded from
`GameRuntime` only if under `debug/`/`editor/` — note `editor/` IS excluded from
the runtime by the existing `FILTER EXCLUDE REGEX "[/\\]editor[/\\]"`, which is
correct: the runtime must not bake paks).

Run: `cmake --build build-ninja-clang --target App`
Expected: builds cleanly.

- [ ] **Step 8: Manual verification**

Run the editor from the build tree (per project convention, from
`build-ninja-clang`), open a project, and:
1. Click **Rebuild Engine Pak** → status shows "Baked engine.pak (N KB)." and
   `build-ninja-clang/data/engine.pak` mtime updates.
2. Click **Publish** → published build contains a `data/engine.pak` and launches.

Expected: both succeed with no console error.

- [ ] **Step 9: Commit**

```bash
git add src/app/CMakeLists.txt src/app/editor/EditorEnginePak.hpp src/app/editor/EditorEnginePak.cpp src/app/editor/EditorProjectActions.hpp src/app/editor/EditorProjectPublisher.cpp src/app/debug/EditorProjectManager.cpp src/app/debug/ProjectPanel.cpp
git commit -m "$(printf 'Bake engine.pak in-process from the editor\n\nDev-gated on AETHER_ENGINE_RESOURCES_DIR: Publish auto-bakes a fresh\nengine.pak (copy fallback for a shipped editor) plus a Rebuild Engine\nPak action.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Task 7: Pak format v2 — validated index (§7)

Add an index hash and a wider header, and make the reader bounds-check every
offset and verify the index before trusting it. Bump `PAK_VERSION` so stale paks
are rejected and rebuilt.

**Files:**
- Modify: `include/PakFormat.hpp`
- Modify: `tools/assetpack/PakWriter.cpp`
- Modify: `src/engine/io/PakBackend.cpp`
- Create: `tests/assetpack/PakFormatTests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Widen the header and bump the version**

In `include/PakFormat.hpp`, change `PAK_VERSION` and the `PakHeader` struct:

```cpp
inline constexpr uint32_t PAK_VERSION   = 2;
```

Replace `PakHeader` with the 64-byte layout:

```cpp
struct PakHeader
{
	char     magic[4]        = { 'A', 'E', 'P', 'K' };
	uint32_t version         = PAK_VERSION;
	uint32_t numEntries      = 0;
	uint32_t flags           = 0; // pak-level flags (reserved, must be 0 for now)
	uint64_t pathDataOffset  = 0;
	uint64_t pathDataSize    = 0;
	uint64_t assetDataOffset = 0;
	uint64_t assetDataSize   = 0;
	uint64_t indexHash       = 0; // XXH3-64 of (entry table bytes ++ path-data bytes)
	uint64_t headerReserved  = 0; // reserved, must be 0
};

static_assert(sizeof(PakHeader) == 64);
```

(Leave `PakEntry` unchanged at 40 bytes.)

- [ ] **Step 2: Compute and write `indexHash` in the writer**

In `tools/assetpack/PakWriter.cpp`, in `Write()`, after the entry table and
`pathData` are fully built and before constructing `PakHeader header;` (around
line 264), the header now needs the index hash. Since `indexHash` covers the
entry table + path data, compute it from those two buffers. Change the header
construction block to:

```cpp
		const uint64_t entryTableSize = sizeof(PakEntry) * entries.size();
		const uint64_t pathDataOffset = sizeof(PakHeader) + entryTableSize;
		const uint64_t assetDataOffset = pathDataOffset + static_cast<uint64_t>(pathData.size());

		// Index hash = XXH3-64 over the entry table bytes followed by the path blob.
		XXH3_state_t* xstate = XXH3_createState();
		XXH3_64bits_reset(xstate);
		XXH3_64bits_update(xstate, entries.data(), static_cast<size_t>(entryTableSize));
		XXH3_64bits_update(xstate, pathData.data(), pathData.size());
		const uint64_t indexHash = XXH3_64bits_digest(xstate);
		XXH3_freeState(xstate);

		PakHeader header;
		header.numEntries = static_cast<uint32_t>(entries.size());
		header.pathDataOffset = pathDataOffset;
		header.pathDataSize = static_cast<uint64_t>(pathData.size());
		header.assetDataOffset = assetDataOffset;
		header.assetDataSize = static_cast<uint64_t>(assetData.size());
		header.indexHash = indexHash;
```

(`PakWriter.cpp` already includes `<PakFormat.hpp>` and `AssetProcessor.hpp`,
which pulls in `<xxhash.h>` with `XXH_INLINE_ALL` — `XXH3_createState` etc. are
available.)

- [ ] **Step 3: Validate bounds + index hash in the reader**

In `src/engine/io/PakBackend.cpp`, in the constructor, after the version check
(around line 101) and before reading the entry table, add bounds validation, then
after reading entries + pathData, verify the index hash. Replace the section from
the version check through the `m_assetDataBase = header.assetDataOffset;` line
with:

```cpp
		if (header.version != PAK_VERSION)
		{
			throw FileSystemError("Unsupported pak version (" + std::to_string(header.version) + ", expected " + std::to_string(PAK_VERSION) + ") in: " + m_pakPath.string());
		}

		// Validate the index against the real file size before trusting any offset.
		std::error_code sizeEc;
		const std::uintmax_t fileSize = std::filesystem::file_size(m_pakPath, sizeEc);
		if (sizeEc)
		{
			throw FileSystemError("Cannot stat pak file: " + m_pakPath.string());
		}

		constexpr uint32_t kMaxEntries = 8u * 1024u * 1024u; // 8M entries sanity cap
		const uint64_t entryTableSize = static_cast<uint64_t>(header.numEntries) * sizeof(PakEntry);
		const uint64_t indexEnd = sizeof(PakHeader) + entryTableSize;
		if (header.numEntries > kMaxEntries
		    || indexEnd > fileSize
		    || header.pathDataOffset != indexEnd
		    || header.pathDataOffset + header.pathDataSize > fileSize
		    || header.assetDataOffset < header.pathDataOffset + header.pathDataSize
		    || header.assetDataOffset + header.assetDataSize > fileSize)
		{
			throw FileSystemError("Corrupt pak index (offsets out of range) in: " + m_pakPath.string());
		}

		// Read entry table.
		std::vector<PakEntry> entries(header.numEntries);
		pak.read(reinterpret_cast<char*>(entries.data()), static_cast<std::streamsize>(entryTableSize));

		// Read path-data section.
		std::vector<char> pathData(static_cast<std::size_t>(header.pathDataSize));
		pak.seekg(static_cast<std::streamoff>(header.pathDataOffset));
		pak.read(pathData.data(), static_cast<std::streamsize>(header.pathDataSize));

		if (!pak)
		{
			throw FileSystemError("Failed to read pak index from: " + m_pakPath.string());
		}

		// Verify the index hash covers the entry table + path blob unmodified.
		{
			XXH3_state_t* xstate = XXH3_createState();
			XXH3_64bits_reset(xstate);
			XXH3_64bits_update(xstate, entries.data(), static_cast<size_t>(entryTableSize));
			XXH3_64bits_update(xstate, pathData.data(), pathData.size());
			const uint64_t actualIndexHash = XXH3_64bits_digest(xstate);
			XXH3_freeState(xstate);
			if (actualIndexHash != header.indexHash)
			{
				throw FileSystemError("Corrupt pak index (hash mismatch) in: " + m_pakPath.string());
			}
		}

		m_assetDataBase = header.assetDataOffset;
```

The replacement above fully subsumes the original entry-table/pathData read block
(version check → `m_assetDataBase`), so no separate deletion is needed — just
overwrite that whole span.

Then bounds-check per-entry ranges where the index is built. The
`for (const auto& e: entries)` loop follows `m_assetDataBase = ...` (around line
120); inside it, before `m_index.emplace`, add (uint64 casts avoid uint32
overflow on the sums):

```cpp
			if (static_cast<uint64_t>(e.pathOffset) + e.pathLen > header.pathDataSize
			    || static_cast<uint64_t>(e.dataOffset) + e.dataSize > header.assetDataSize)
			{
				throw FileSystemError("Corrupt pak entry (range out of bounds) in: " + m_pakPath.string());
			}
```

- [ ] **Step 4: Write format-hardening tests (TDD)**

Create `tests/assetpack/PakFormatTests.cpp`:

```cpp
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "AssetPipeline.hpp"
#include "io/PakBackend.hpp"
#include "utils/AetherExceptions.hpp"

using namespace aether;

namespace
{
	std::filesystem::path MakeValidPak(const std::string& tag)
	{
		const std::filesystem::path src = std::filesystem::temp_directory_path() / ("aepak_fmt_src_" + tag);
		std::filesystem::remove_all(src);
		std::filesystem::create_directories(src);
		std::ofstream(src / "a.txt", std::ios::binary) << "content";

		const std::filesystem::path pak = std::filesystem::temp_directory_path() / ("aepak_fmt_" + tag + ".pak");
		std::filesystem::remove(pak);
		const auto result = assetpipeline::PackDirectory(src, pak, {});
		REQUIRE(result.ok);
		return pak;
	}

	std::vector<char> ReadAll(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary | std::ios::ate);
		std::vector<char> buf(static_cast<std::size_t>(in.tellg()));
		in.seekg(0);
		in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
		return buf;
	}

	void WriteAll(const std::filesystem::path& p, const std::vector<char>& buf)
	{
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
	}
}

TEST_CASE("A valid pak opens")
{
	const auto pak = MakeValidPak("valid");
	CHECK_NOTHROW(io::PakBackend{pak});
}

TEST_CASE("A corrupted index byte is rejected")
{
	const auto pak = MakeValidPak("corrupt");
	auto bytes = ReadAll(pak);
	// Flip a byte inside the entry table (just past the 64-byte header).
	bytes.at(70) ^= 0xFF;
	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}

TEST_CASE("A truncated pak is rejected")
{
	const auto pak = MakeValidPak("trunc");
	auto bytes = ReadAll(pak);
	bytes.resize(bytes.size() / 2); // lop off the asset data
	WriteAll(pak, bytes);
	CHECK_THROWS_AS(io::PakBackend{pak}, FileSystemError);
}
```

Add to `tests/CMakeLists.txt` `EngineTests` sources:

```cmake
    assetpack/PakFormatTests.cpp
```

- [ ] **Step 5: Build and run the format tests**

Run: `cmake --build build-ninja-clang --target EngineTests && build-ninja-clang/tests/EngineTests.exe --test-case="A valid pak opens,A corrupted index byte is rejected,A truncated pak is rejected"`
Expected: all three PASS. (`FileSystemError` type confirmed in
`src/engine/utils/AetherExceptions.hpp` — verify the include path with
`grep -rn "class FileSystemError\|struct FileSystemError" src/engine`.)

- [ ] **Step 6: Rebuild the build-tree paks and full suite**

Run: `cmake --build build-ninja-clang --target App`
Expected: the POST_BUILD `engine.pak`/`project.pak` steps rebuild v2 paks; the
app's own runtime load of `engine.pak` at startup still works (verify by running
the editor briefly — it loads fonts from engine.pak).
Run: `ctest --test-dir build-ninja-clang -R EngineTests --output-on-failure`
Expected: entire suite passes.

- [ ] **Step 7: Commit**

```bash
git add include/PakFormat.hpp tools/assetpack/PakWriter.cpp src/engine/io/PakBackend.cpp tests/assetpack/PakFormatTests.cpp tests/CMakeLists.txt
git commit -m "$(printf 'Add validated pak index (format v2)\n\n64-byte header carries an XXH3 index hash; the reader bounds-checks all\noffsets and verifies the index before trusting a pak. Rejects corrupt or\ntruncated paks cleanly instead of OOB reads.\n\nCo-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>')"
```

---

## Final verification

- [ ] **Full build, both configs the CI uses:**
  Run: `cmake --build build-ninja-clang --target App GameRuntime AssetPacker EngineTests`
  Expected: all targets build; `GameRuntime` links without `AssetPipeline` (grep
  its link line to confirm the runtime stays lean).

- [ ] **Full test suite:**
  Run: `ctest --test-dir build-ninja-clang -R EngineTests --output-on-failure`
  Expected: 100% pass.

- [ ] **End-to-end editor check (the original bug):**
  Launch the editor, open a project, click **Pack Project** then **Publish**.
  Expected: both succeed — no "The filename, directory name, or volume label
  syntax is incorrect." The published build launches.

- [ ] **MSVC parity (production toolchain):**
  Run: `cmake --build build-vs2022-msvc --target App EngineTests` (configure the
  preset first if that tree is stale). Expected: builds and tests pass under MSVC.

---

## Self-Review notes (for the executor)

- **stb collision** is the single duplicate-symbol risk; Task 1 resolves it by
  keeping the stb impl exe-only. If `App` still hits a duplicate `stbi_*` symbol,
  confirm `Texture.cpp` uses default (external) linkage and that `ThirdPartyImpl.cpp`
  no longer defines `STB_IMAGE_IMPLEMENTATION`.
- **cgltf/xxhash/bc7enc** are safe (engine defines none as external header-impls;
  xxhash in the engine is `XXH_INLINE_ALL`, internal linkage).
- **Type consistency:** `PackResult`/`PackOptions` field names in Task 3 are used
  verbatim in Tasks 4 and 6. `EditorProjectActionResult{.succeeded, .message,
  .outputPath}` is the existing editor result type (do not confuse with the
  library's `PackResult{.ok, ...}`).
- **Ordering:** Task 5 removes `ShellQuotePath`/`RunCommandToLog`; Task 4 leaves
  them in place deliberately. Do not delete them in Task 4.
```
