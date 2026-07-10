# In-Process Asset Pipeline (`AssetPipeline` library)

**Date:** 2026-07-10
**Status:** Design — awaiting approval

**Sequencing:** Core (in-process pack + engine.pak) plus §6 (process helper) and
§7 (pak format v2) land together as one feature on one branch / one
implementation plan.

## Problem

Publishing or packing a project from the editor fails on Windows with:

> The filename, directory name, or volume label syntax is incorrect.

Root cause: `EditorProjectPublisher` bakes paks by building a shell string and
calling `std::system` ([EditorProjectPublisher.cpp:588](../../../src/app/editor/EditorProjectPublisher.cpp)).
The command starts with a quoted exe path and contains several more quoted
paths (source, output, redirect target). On Windows `std::system` runs
`cmd.exe /c <string>`; with more than two quote characters cmd strips the first
and last quote, leaving `C:\...\AssetPacker.exe"` (a name with a trailing quote)
— an invalid filename, hence error 123 (`ERROR_INVALID_NAME`). The same latent
bug exists in `RunCommandToLog` used for the `dotnet build` step.

Beyond the specific bug, the editor shelling out to a separate `AssetPacker.exe`
is fragile: it must *locate* the exe on disk (`FindAssetPackerExecutable` probes
six candidate paths), parse a log file for errors, and depend on process/quoting
behaviour. Now that the editor exists, the packer no longer needs to be a
separate process — its logic can be a library the editor links and calls
directly, in-process.

## Goals

- Editor packs and publishes **in-process** — no `std::system`/`cmd.exe` for
  packing. Eliminates the entire quoting failure class.
- AssetPacker becomes a **library** (`AssetPipeline`, the single source of truth
  for pak production), with a **thin CLI** retained only for the CMake
  build-time bake and CI/headless use.
- Editor can bake **`engine.pak`** in-process too (dev checkouts), not just
  `project.pak`.
- Fix the one remaining shell-out (`dotnet build`) by routing it through a
  single correct cross-platform process helper (no duplicate quoting logic).
- Harden the `.pak` format so a pak the reader did not just produce (e.g. one
  the editor shipped to another machine) is validated before it is trusted.

## Non-Goals

- No plugin/DLL loading (`LoadLibrary`/`dlopen`). Static linking is simpler,
  type-checked, single-binary, and is the standard editor-module pattern.
  A runtime DLL would reintroduce the same "find the binary on disk" fragility.
- The shipped **`GameRuntime`** does not link `AssetPipeline`. The runtime only
  *reads* paks (`PakBackend`); it stays lean (no freetype/cgltf/bc7enc).
- Not relocating packer sources out of `tools/`, not reworking the reader's O(n)
  scans, not adding per-entry asset-kind metadata, not replacing the text
  manifest with a binary block, not adding endianness markers. (See §7 for why.)

## Architecture

### 1. Target split

`tools/assetpack/CMakeLists.txt` currently produces a single `add_executable`.
Split into:

| Target | Kind | Contents |
| --- | --- | --- |
| `AssetPipeline` | `STATIC` library | All logic: `PakWriter`, `TextureProcessor`, `MeshProcessor`, `MaterialProcessor`, `SpirvProcessor`, `FontProcessor`, `MaterialImporter`, `ThreadPool`, `PakLog`, `PakManifest`. Third-party non-stb impls (cgltf, bc7enc, xxhash) compiled here. |
| `AssetPacker` | executable | Thin `main.cpp` (arg parsing) + the stb-image impl TU. Links `AssetPipeline`. Kept for CMake POST_BUILD bake and CI. |

- The editor target **`App`** links `AssetPipeline` (`PRIVATE`). `GameRuntime`
  does not.
- Packer symbols move from the **global namespace** into
  **`aether::assetpipeline`**. This removes the global `namespace fs =
  std::filesystem;` aliases currently sitting at file scope in the packer
  headers, which would otherwise leak into every engine translation unit that
  includes them.
- Packer source files physically stay in `tools/assetpack/` (minimises include
  churn); only the CMake target structure and namespacing change.

### 2. Third-party symbol de-duplication (the one real collision)

Investigated: of the packer's single-header dependencies, only **stb_image**
collides with the engine.

| Library | Engine uses it? | Handling |
| --- | --- | --- |
| stb_image | Yes — `STB_IMAGE_IMPLEMENTATION` in `Texture.cpp` | **Collision.** `AssetPipeline` references but does **not** define stb. The standalone `AssetPacker` exe provides the impl in its own TU; `App` reuses the engine's existing `stbi_*` exports. |
| cgltf | No (engine has no cgltf) | Compiled in `AssetPipeline`; no collision anywhere. |
| xxhash | Yes, but `XXH_INLINE_ALL` (internal linkage) | No collision — inline-all symbols are internal; the packer's `XXH_IMPLEMENTATION` externals coexist. |
| bc7enc / rgbcx | No | Compiled in `AssetPipeline`. |
| zstd, freetype, glm, tomlplusplus | Proper link libraries (some via Engine) | Linker de-dupes static libs; no issue. |

Implementation checkpoint: confirm the engine's stb build flags (default,
external linkage, no `STB_IMAGE_STATIC`) match what the packer TUs expect.

### 3. Public library API

New header `tools/assetpack/AssetPipeline.hpp` — the editor-facing surface.
Result-returning (no stdout/log-file parsing by callers):

```cpp
namespace aether::assetpipeline
{
    struct PackOptions
    {
        int  compressionLevel = 3;
        bool importMaterials  = false; // run MaterialImporter first (project packs)
        bool projectLayout    = false; // apply project exclusion rules + descriptor check
    };

    struct PackResult
    {
        bool          ok = false;
        std::string   message;      // human-readable summary or failure reason
        std::size_t   entryCount = 0;
        std::uintmax_t pakBytes  = 0;
        std::filesystem::path outputPath;
    };

    // Pack a project root (materials import + project exclusion rules).
    PackResult PackProject(const std::filesystem::path& projectRoot,
                           const std::filesystem::path& outputPak,
                           const PackOptions& options);

    // Pack an arbitrary asset directory (used for engine.pak).
    PackResult PackDirectory(const std::filesystem::path& sourceDir,
                             const std::filesystem::path& outputPak,
                             const PackOptions& options);
}
```

`.log` and `.manifest` sidecar files continue to be written to disk (the publish
step copies them and the incremental manifest drives rebuild skipping), but the
`PackResult` is the authoritative signal for callers — no reading a log file
back to discover success/failure. `main.cpp` becomes a thin translation of argv
into these calls.

### 4. Editor integration — in-process pack

In `EditorProjectPublisher.cpp`:

- `PackProjectInternal` replaces its `std::system(...)` block with a direct call
  to `aether::assetpipeline::PackProject(...)`.
- Delete now-dead code: `ShellQuotePath`, `ReadLogExcerpt` (for the pack path),
  `FindAssetPackerExecutable`, and the `assetPackerExe` field on
  `EditorProjectPublishConfig` / its use in `MakeDefaultEditorProjectPublishConfig`.
- The "could not find AssetPacker.exe" failure path disappears entirely.

This is the change that fixes the reported bug.

### 5. Editor bakes `engine.pak`

`engine.pak` is baked from engine-owned source resources (`resources/fonts`,
plus `resources/config` copied separately) — see
`aethercore_add_runtime_payload` in
[src/app/CMakeLists.txt:165](../../../src/app/CMakeLists.txt). With
`PackDirectory` available in-process the editor can bake it too.

- Gated behind a new compile define `AETHER_ENGINE_RESOURCES_DIR` (present only
  in dev checkouts, mirroring the existing `AETHER_GAME_PROJECT` /
  `AETHER_DOTNET_EXE` gating). A *shipped* editor has no `resources/` tree.
- **Publish**: when the define is present, regenerate a fresh `engine.pak` from
  source into the published build; when absent, fall back to copying the
  existing `data/engine.pak` (shipped editor keeps working — no regression).
- **Tools ▸ Rebuild Engine Pak** menu action (in-process, immediate), alongside
  the Publish auto-bake.
- CMake **keeps** baking `engine.pak` at build time (via the thin CLI) so
  `cmake --build` remains self-sufficient and the editor always has a pak to
  launch from. No bootstrap paradox.

### 6. Single process helper for the unavoidable shell-out

Packing goes in-process; the only external process left is `dotnet build`
(project-script publish, and the F5 hot-reload in `CSharpScriptingSubsystem`).
There is no in-proc API for the .NET SDK, so this stays a subprocess — but the
quoting is done once, correctly.

- Introduce one small helper in the `aether::io` layer, e.g.
  `RunProcessToLog(command, logPath)` / `RunProcessCapture(command, &output)`,
  with the correct per-platform quoting (on Windows, wrap the whole command in
  an outer quote pair so cmd's first/last-quote stripping leaves the real
  command intact — the proven pattern already in
  `CSharpScriptingSubsystem::RunCapture`).
- Route both `EditorProjectPublisher`'s `dotnet build` and
  `CSharpScriptingSubsystem`'s hot-reload build through it, deleting the
  duplicated (and, in the publisher, buggy) quoting logic.

### 7. `.pak` format hardening (PAK_VERSION 1 → 2)

Newly relevant: the editor is about to produce paks in-process and ship them to
other machines, so the reader must validate paks it did not just produce. Today
per-entry `contentHash` covers each asset payload, but the header, the
`PakEntry[]` table, and the path blob have **no checksum**, and the reader
trusts `header.numEntries` and every offset blindly
([PakBackend.cpp:104](../../../src/engine/io/PakBackend.cpp)).

Changes:

- **Header → 64 bytes** (from 48). Repurpose the unused `reserved` (header) and
  `_pad` (entry) space:
  - `uint32_t flags` replaces `reserved` (pak-level flags, future use).
  - Add `uint64_t indexHash` = XXH3-64 of (entry table bytes ++ path-blob bytes).
  - Add `uint64_t headerReserved` (zeroed) to round to 64 and leave growth room.
- **Reader validation** before building the index:
  - Stat the real file size; reject if `numEntries`, entry-table span,
    `pathDataOffset+pathDataSize`, or `assetDataOffset+assetDataSize` fall
    outside the file, or if `numEntries` exceeds a sane cap.
  - Recompute and verify `indexHash`; reject on mismatch with a clear
    "corrupt pak index" error.
- `PakEntry._pad` stays reserved (kept zero) — 40-byte entry size unchanged.
- Bump `PAK_VERSION` to 2; the reader's existing version check rejects stale
  paks with the existing "rebuild assets" message. No migration: paks are build
  artifacts, always regenerated from source.

Rejected (YAGNI): per-entry asset-kind metadata, binary metadata block, reader
binary-search/perf rework, endianness markers.

### 8. CMake changes

- `tools/assetpack/CMakeLists.txt`: `add_library(AssetPipeline STATIC ...)` +
  `add_executable(AssetPacker main.cpp stb_impl.cpp)` linking it. Move the
  include dirs / link libs onto `AssetPipeline` (mostly `PRIVATE`; the public
  API header needs only `<filesystem>`/`<string>`, so `AssetPipeline`'s
  interface stays clean).
- `src/app/CMakeLists.txt`: `target_link_libraries(App PRIVATE AssetPipeline)`;
  add `AETHER_ENGINE_RESOURCES_DIR` define for `App`. POST_BUILD `engine.pak`
  and `project.pak` bakes continue to use `$<TARGET_FILE:AssetPacker>` unchanged.
- `aethercore_enable_dead_strip_report` continues to apply to `AssetPacker`.

### 9. Testing

Splitting logic into a library makes it unit-testable for the first time.

- New `PackProject`/`PackDirectory` round-trip test: pack a fixture directory,
  read it back through `PakBackend`, assert entry set, sizes, and hashes.
- Format-hardening tests: a pak with a corrupted index hash, a truncated file,
  and an out-of-range offset each fail cleanly (no crash/OOM).
- `PackMaterialTests` / `TextureRegistryTests` link `AssetPipeline` directly
  instead of duplicating logic.
- Editor smoke: `PublishProject` on a fixture project succeeds end-to-end with
  no subprocess for packing.

## Risks & Mitigations

| Risk | Mitigation |
| --- | --- |
| stb build-flag mismatch between engine and packer TUs | Verify default external linkage; `AssetPipeline` defines no stb impl, so App has exactly one definition (the engine's). |
| Global→namespace churn touches many files | Mechanical; contained to `tools/assetpack/` + call sites. Compiler catches misses. |
| `AETHER_ENGINE_RESOURCES_DIR` absent in some dev configs | Publish falls back to copying existing `engine.pak`; behaviour identical to today. |
| Format v2 read by an old engine | Version check already rejects with a clear rebuild message; both sides share `PakFormat.hpp`. |
| Scope creep from §6/§7 | Both are tightly bounded and directly serve the "AAA/consolidated/robust" goal; each can be dropped independently without affecting the core fix. |

## Definition of Done

- Publish/Pack from the editor succeed on Windows with no `std::system` for
  packing; the error 123 failure class is gone.
- `AssetPipeline` static library exists; `AssetPacker` is a thin CLI over it;
  `GameRuntime` does not link the pipeline.
- Editor can bake `engine.pak` in dev checkouts; shipped editor falls back to
  copy.
- Single process helper handles the `dotnet build` shell-out; no duplicated
  quoting logic.
- `.pak` v2 with validated index; corrupt/truncated paks fail cleanly.
- Tests above pass.
