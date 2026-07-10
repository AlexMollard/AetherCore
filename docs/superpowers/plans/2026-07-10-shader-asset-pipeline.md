# Shader Asset Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Fresh implementer per task + spec-compliance review then code-quality review. Build/verify under MSVC (`build-vs2022-msvc`); `build-ninja-clang` is a valid secondary but MSVC is the source of truth here.

**Goal:** Make shaders first-class pak assets flowing through the asset pipeline (author → compile → pack → load), with engine shaders in `engine.pak`, project/custom-material shaders authored in projects and compiled by the editor, and a unified `shaders://` overlay that resolves project-over-engine. This properly fixes the published-game startup crash (missing shaders) and enables custom material shaders.

**Architecture:** Every material already references its shader by a VFS path (`shaderVfsPath = "shaders://x.spv"`). We keep that contract and change where the path resolves: `shaders://` becomes an `OverlayBackend` (a new `IFileBackend`) layering project shaders over engine shaders. Engine shaders compile at build time and pack into `engine.pak:shaders/`; project shaders compile in the editor (slangc via `aether::io::RunProcess`, hot-reload) and pack into `project.pak:shaders/`. In dev the overlay's sub-backends are directories; when shipped they're pak sub-trees.

**Tech Stack:** C++20, CMake, Vulkan/SPIR-V, slangc, doctest, `aether::io` VFS (`IFileBackend`/`DirectoryBackend`/`PakBackend`), `aether::assetpipeline` (`PakWriter`/`SpirvProcessor`), `aether::io::RunProcess` (Task 5 helper).

**Decisions (from brainstorming):** editor compiles project shaders on import/change (hot-reload); unified `shaders://` overlay (project overrides engine); build the whole thing now; project shader sources live at `<project>/assets/shaders/*.slang`.

---

## Key reference files (implementers: read these)
- `src/engine/io/IFileBackend.hpp` — the 5-virtual backend contract (`Exists`/`Read`/`OpenStream`/`Glob`/`Write`).
- `src/engine/io/DirectoryBackend.{hpp,cpp}`, `PakBackend.{hpp,cpp}` — the two existing backends.
- `src/engine/io/FileSystem.cpp` — `InitializeDefaultMounts` (engine://, project://, shaders:// setup ~L263-395), `ResolveMountedDirectory`, `Mount`, mode env vars (`AETHER_ENGINE_MODE=pak|dir|auto`).
- `src/app/scripting/CSharpScriptingSubsystem.cpp` — `RebuildFromSource` (the compile-if-stale + hot-reload pattern to mirror for shaders), and how slangc/dotnet defines are consumed.
- `src/engine/io/Process.hpp` — `aether::io::RunProcessToLog/RunProcessCapture` (Task 5).
- `CMake/SlangShaders.cmake` — `SLANGC_EXECUTABLE`, `AETHERCORE_SLANG_SHADER_ARGS`, `AETHERCORE_SHADER_OUTPUT_DIR`, `aethercore_enable_slang_shader_compilation`.
- `src/app/editor/EditorEnginePak.cpp` — `BakeEnginePak`/`kEngineAssetSubdirs` (add shaders staging).
- `src/app/CMakeLists.txt` — App compile defs + `aethercore_add_runtime_payload` (engine.pak bake) in `src/app/CMakeLists.txt`.
- `tools/assetpack/PakWriter.{hpp,cpp}`, `AssetProcessor.hpp` (SpirvProcessor path), `AssetPipeline.{hpp,cpp}` — pack API.
- `src/engine/material/MaterialTemplate.hpp`, `MaterialAsset.hpp`, `tools/assetpack/MaterialProcessor.cpp` — material→shader link + properties.toml parsing.
- `include/PakFormat.hpp` — `PAK_PIPELINE_VERSION` (bump when pak content model changes).

---

## Phase 1 — `OverlayBackend` (VFS composite)

**Goal:** A backend that layers ordered sub-backends, each with an optional path prefix, so `shaders://x.spv` can map to `engine.pak:shaders/x.spv` (prefix) or `build/shaders/x.spv` (no prefix), with project overriding engine.

**Files:** Create `src/engine/io/OverlayBackend.hpp`/`.cpp`; create `tests/io/OverlayBackendTests.cpp`; add the test to `tests/CMakeLists.txt`.

**Contract:**
```cpp
namespace aether::io {
class OverlayBackend final : public IFileBackend {
public:
    struct Layer { std::shared_ptr<IFileBackend> backend; std::string prefix; }; // prefix prepended to the relative path
    explicit OverlayBackend(std::vector<Layer> layers);          // index 0 = highest priority
    bool Exists(std::string_view rel) const override;            // true if any layer has it
    Expected<std::vector<std::byte>> Read(std::string_view rel) const override;       // first layer that has it
    Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view rel) const override; // first
    Expected<std::vector<std::string>> Glob(std::string_view pattern, const FileGlobOptions&) const override; // union across layers, de-dup by result path, higher-priority layer wins; results returned WITHOUT the layer prefix (mount-relative)
    Expected<void> Write(std::string_view rel, std::span<const std::byte>) const override; // delegate to first layer (write-through to highest priority)
};
}
```
Notes for the implementer: prepend `prefix` to `rel` before delegating to a layer's backend; for `Glob`, prepend prefix to the pattern per layer and strip it from returned paths so overlay results are mount-relative and consistent across layers. On a Read/OpenStream miss in a layer, fall through to the next; only return the last error if all miss.

**Steps (TDD):**
- [ ] Write `tests/io/OverlayBackendTests.cpp` first: two temp dirs each wrapped in `DirectoryBackend`, wrapped in an `OverlayBackend` (empty prefixes). Cases: (a) file only in low-priority layer is read; (b) file in both layers reads the HIGH-priority copy (override); (c) `Glob("**/*.txt")` unions both and de-dups an overlapping name to one entry; (d) a prefixed layer (`prefix="shaders/"`) resolves `x.spv` to `<dir>/shaders/x.spv`.
- [ ] Run tests, see them fail to link.
- [ ] Implement `OverlayBackend`. Register via engine source glob (confirm `src/engine/CMakeLists.txt` globs `io/*.cpp`; if explicit, add it).
- [ ] Build `EngineTests` (MSVC) and confirm the new cases pass; full suite green.
- [ ] Commit.

---

## Phase 2 — Engine shaders → `engine.pak` + `shaders://` overlay (fixes publish)

**Goal:** Engine shaders ship inside `engine.pak` and load through the overlay; the published game runs. Removes the need for the reverted loose-copy patch.

**Files:** `src/app/CMakeLists.txt` (define + engine.pak bake), `src/app/editor/EditorEnginePak.cpp` (stage shaders), `src/engine/io/FileSystem.cpp` (shaders:// overlay).

**Steps:**
- [ ] **CMake define.** In App's `target_compile_definitions`, add `AETHER_SHADER_BUILD_DIR="${AETHERCORE_SHADER_OUTPUT_DIR}"` (dev-gated; the compiled-`.spv` dir).
- [ ] **engine.pak bake (CMake).** In `aethercore_add_runtime_payload` (`src/app/CMakeLists.txt`), before packing `engine_assets`, also copy `${AETHERCORE_SHADER_OUTPUT_DIR}` → `${CMAKE_BINARY_DIR}/engine_assets/shaders`. Result: `engine.pak` contains `fonts/` + `shaders/`. (Add a dependency so shaders compile before this copy — `${target}_CompileShaders`.)
- [ ] **BakeEnginePak (editor).** In `EditorEnginePak.cpp`, stage compiled shaders alongside fonts: fonts come from `AETHER_ENGINE_RESOURCES_DIR/fonts`; shaders come from `AETHER_SHADER_BUILD_DIR` → staging `shaders/`. Generalize the staging (two sources, different roots). `CanBakeEnginePak` stays true only if BOTH the resources dir and the shader build dir resolve.
- [ ] **shaders:// overlay wiring.** In `FileSystem::InitializeDefaultMounts`, replace the loose `shaders://` mount with an `OverlayBackend`:
  - Determine engine mode the same way engine:// does (pak vs dir).
  - **dir/dev:** layer = `{ DirectoryBackend(resolved build shaders dir, as ResolveMountedDirectory finds today), prefix="" }`.
  - **pak/shipped:** layer = `{ <the engine.pak backend>, prefix="shaders/" }` (reuse the same `PakBackend` instance mounted for `engine://`, or construct one for the engine pak path; prefix maps `shaders://x.spv` → `engine.pak:shaders/x.spv`).
  - Project shader layer is prepended in Phase 4; leave a clear extension point.
- [ ] **Build** App + GameRuntime (MSVC). **Verify engine.pak content:** after building, `engine.pak` must contain `shaders/*.spv` (inspect via the pak log/manifest or a quick VFS glob). The 25 engine shaders must be present.
- [ ] **End-to-end verify (the crash fix):** Publish the `projects/TestingProject` build (or reproduce the publish output), confirm `data/engine.pak` contains `shaders/`, and launch the published `AetherGame.exe`; its log must progress PAST `BindlessManager` (previously it access-violated there). If GPU-risk is a concern, at minimum confirm via `shaders://**/*.spv` glob at runtime returns the engine shaders. Report the log tail.
- [ ] Commit.

---

## Phase 3 — Editor `ShaderCompiler` (slangc, hot-reload)

**Goal:** The editor compiles a project's `<project>/assets/shaders/*.slang` → `.spv` on import/change, mirroring the C# F5 flow, and the dev `shaders://` overlay serves them (project overrides engine).

**Files:** `src/app/CMakeLists.txt` (slangc defines), new `src/app/scripting/ShaderCompiler.{hpp,cpp}` (or `src/app/editor/`), wiring into the editor's asset-watch/reload, `src/engine/io/FileSystem.cpp` (project shader layer in dev).

**Steps:**
- [ ] **CMake slangc plumbing.** Add to App defs (dev-gated, near the dotnet block): `AETHER_SLANGC_EXE="${SLANGC_EXECUTABLE}"`, `AETHER_SLANG_ARGS="${AETHERCORE_SLANG_SHADER_ARGS}"`. Also a project shader intermediate convention (compiled to `<project>/Builds/Intermediate/shaders/`).
- [ ] **ShaderCompiler.** New subsystem modeled on `CSharpScriptingSubsystem::RebuildFromSource`: for each `<project>/assets/shaders/*.slang`, if stale (mtime vs output), run `"${AETHER_SLANGC_EXE}" <args> <in.slang> -o <out.spv>` via `aether::io::RunProcessToLog`, output to `<project>/Builds/Intermediate/shaders/<name>.spv`. Provide `CompileAll(projectRoot)` (returns a result + log) and a `CompileIfStale` used by hot-reload. Gate on `AETHER_SLANGC_EXE` being defined (dev).
- [ ] **Hot-reload wiring.** Trigger `CompileIfStale` when a project `.slang` changes (hook into the editor's existing asset-watch / the same mechanism that hot-reloads C#/assets — read how that is wired). A manual "Recompile Shaders" action is acceptable if a file-watch hook isn't readily available; note which was used.
- [ ] **Dev overlay layer.** In `FileSystem::InitializeDefaultMounts`, when a project intermediate shaders dir exists (dev), PREPEND `{ DirectoryBackend(<project>/Builds/Intermediate/shaders), prefix="" }` as the highest-priority `shaders://` layer, so a project shader overrides an engine shader of the same name and new project shaders resolve.
- [ ] **Verify:** In `projects/TestingProject`, add `assets/shaders/plasma.slang` (or reuse an existing sample), trigger compile, confirm `<project>/Builds/Intermediate/shaders/plasma.spv` is produced and `shaders://plasma.spv` resolves to it at editor runtime. Report.
- [ ] Commit.

---

## Phase 4 — Publish compiles + packs project shaders into `project.pak`

**Goal:** Publishing compiles project shaders (clean, like the dotnet build) and packs the `.spv` into `project.pak:shaders/`; the shipped overlay serves them.

**Files:** `tools/assetpack/PakWriter.{hpp,cpp}` (prefix-add API), `tools/assetpack/AssetPipeline.{hpp,cpp}` (option to include a compiled-shader dir), `src/app/editor/EditorProjectPublisher.cpp` (compile+pack), `src/engine/io/FileSystem.cpp` (project.pak layer when shipped).

**Steps:**
- [ ] **PakWriter prefix API.** Add `PakWriter::AddDirectoryAs(const fs::path& sourceDir, std::string_view vpathPrefix)` (or extend `AddDirectory` with an optional prefix) so a directory's files pack under a virtual prefix (e.g. `shaders/`). Keep the existing sorted/dedup behavior. Unit-test it (pack a dir under `shaders/`, read back a `shaders/x` entry).
- [ ] **AssetPipeline option.** Extend `PackProject`/`PackOptions` with an optional `shaderSpirvDir` (a directory of compiled `.spv`); when set, `PackProject` also `AddDirectoryAs(shaderSpirvDir, "shaders/")` into the same pak. Keep the API minimal.
- [ ] **Publisher.** In `PublishProject`, before packing: if a `ShaderCompiler` is available (dev), clean-compile project shaders to `<project>/Builds/Intermediate/shaders/`; pass that dir as `PackOptions.shaderSpirvDir` so `project.pak` gets `shaders/*.spv`. (Mirror the existing project-scripts dotnet build placement.)
- [ ] **Shipped overlay layer.** In `FileSystem::InitializeDefaultMounts` pak/shipped path, PREPEND `{ <project.pak backend>, prefix="shaders/" }` as the highest-priority `shaders://` layer (project overrides engine).
- [ ] **Verify:** publish a project that has a custom `assets/shaders/*.slang`; confirm `project.pak` contains `shaders/<name>.spv`; the shipped overlay resolves `shaders://<name>.spv` from project.pak. Report the pak manifest lines.
- [ ] Commit.

---

## Phase 5 — Material custom-shader authoring

**Goal:** A project material can declare its shader in `properties.toml` and the runtime builds its pipeline from it.

**Files:** `tools/assetpack/MaterialProcessor.cpp` / `MaterialImporter.cpp` (parse `shader` field), `src/engine/material/MaterialTemplate.hpp` / `MaterialAsset.hpp` (carry the path), and the material-load path that turns a baked material into a `shaderVfsPath`.

**Steps:**
- [ ] **Authoring field.** In the material import/process (`properties.toml` → baked `.material`), read an optional `shader = "shaders://myeffect.spv"` and store it so the runtime material sets `MaterialTemplate.shaderVfsPath` to it (default stays `shaders://gltf_mesh.spv`). Read the existing MaterialProcessor to see how fields map into the baked material blob and thread this through.
- [ ] **Verify:** a project material with `shader = "shaders://<custom>.spv"` (the Phase 3/4 custom shader) resolves to that shader; a material without the field keeps the default. A unit test at the material-describe/pack layer (mirror `tests/material/*`) asserting the shader path round-trips is preferred over a full render.
- [ ] Commit.

---

## Phase 6 — Format bump, cleanup, verify

**Files:** `include/PakFormat.hpp`, `src/app/editor/EditorProjectPublisher.cpp` (VerifyPublishedGame), any dead loose-shader handling.

**Steps:**
- [ ] **Bump `PAK_PIPELINE_VERSION`** (paks now carry shaders — a new content model). The Task-7 manifest cache invalidation (`ManifestHeaderLine`) then auto-forces a repack; confirm engine.pak/project.pak repack.
- [ ] **VerifyPublishedGame.** Replace any loose-`shaders/`-folder assumption: verify the shipped `engine.pak` contains `shaders/*.spv` (e.g. mount+glob, or check the pak manifest), rather than checking a loose folder. Ensure a shader-less publish fails verification.
- [ ] **Remove dead code:** any remaining loose `shaders://`-only resolution superseded by the overlay; ensure no `shaders/` loose-copy remains in the publisher.
- [ ] **Full verify:** build App+GameRuntime+EngineTests (MSVC); full suite green; publish `projects/TestingProject`; launch the published game — it must run past `BindlessManager` and render (the original crash is gone). Report.
- [ ] Commit.

---

## Cross-cutting notes
- **Dev vs shipped** is decided the same way `engine://` decides pak-vs-dir (`AETHER_ENGINE_MODE`); keep `shaders://` consistent with it so the two never disagree.
- **GameRuntime** must keep loading shaders correctly in BOTH dev (dir overlay) and shipped (pak overlay); it does not link `AssetPipeline` and must not need to.
- **Reuse, don't duplicate:** `aether::io::RunProcess` for slangc, `SpirvProcessor` for `.spv` packing, `ResolveMountedDirectory` for dir resolution, the `CSharpScriptingSubsystem` pattern for the compiler.
- **No new shell-out sprawl:** slangc is the one new external process, funneled through `aether::io::RunProcess` (correct quoting already handled).
- Each phase ends green (build + tests) and is independently committable; Phases 1-2 alone fix the published-game crash.
