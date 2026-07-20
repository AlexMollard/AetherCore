# AAA Scene Serializer: Cooked Binary Runtime Format

> **For agentic workers:** Executed inline with commits + tests per phase.

**Goal:** Give scenes and prefabs a compact, versioned **binary** format that the
runtime loads (fast parse, small size), while keeping the reflection-driven TOML
as the human-editable, git-diffable authoring **source**. This is the standard
AAA cooked-asset pattern and the correct answer to "TOML isn't good for large
scenes" — you keep readable, mergeable source and cook a fast binary for runtime,
rather than throwing away a diffable text format.

**Why not just replace TOML:** text source is a feature (code review, merges,
hand-edits, the reflection already generates it losslessly). AAA engines keep a
text/JSON source and cook binary at build time. So: TOML stays the source; binary
is the cooked runtime artifact.

**Architecture:** A format-agnostic intermediate already exists —
`SceneDescription`. Add a binary codec over it (`WriteSceneBinary` /
`ReadSceneBinary`) alongside the existing `WriteToml` / `ParseToml`. Runtime load
prefers `.scene.bin` / `.prefab.bin`; the editor cooks the binary sibling on save;
TOML remains the fallback everywhere, so nothing can break.

**Global Constraints**
- Additive: `SceneDescription`, TOML, and every existing consumer (undo,
  play/stop, prefabs, MCP, tests) are untouched. Binary is a second backend.
- Versioned: magic `AESB` + `uint32` version; unknown/newer versions fail closed
  to the TOML fallback.
- Round-trip exact: `ReadSceneBinary(WriteSceneBinary(d))` must reproduce `d`
  (verified by re-serializing both to TOML and comparing) for every component.
- Master builds + full test suite green after every phase.

---

## Phase 1: BinaryWriter + binary codec + round-trip tests

**Files:**
- Create: `src/engine/utils/BinaryWriter.hpp` (append-only byte writer mirroring BinaryReader)
- Create: `src/app/scene/SceneSerializerBinary.cpp`
- Modify: `src/app/scene/SceneSerializer.hpp` (declare `WriteSceneBinary` / `ReadSceneBinary`)
- Modify: `tests/CMakeLists.txt` (add the new TU)
- Create: `tests/scene/SceneBinaryTests.cpp`

**Design:**
- `BinaryWriter`: `Write<T>()`, `WriteString()` (uint32 length — scenes can hold long strings), `WriteBytes()`, `Bytes()`.
- Binary layout: header (magic+version) → scene meta (name, format version, kind, features) → entity count → per entity (id, parentIndex, name, tags, disabled flag, transform present+pos/euler/scale, then each optional component tag+payload) → lights → environment (optional) → asset manifest. Reflected flat components reuse `reflect::FieldValue` get/set so the payload stays generic; structural/custom records (mesh, material, skinned, physics, scripts, joints, UI, tilemap) get explicit binary I/O mirroring their TOML shape.
- `ReadSceneBinary` validates magic/version, returns `std::nullopt` on mismatch (→ caller falls back to TOML).

**Verify:** round-trip tests build scenes covering every EntityRecord field/component + reuse the TOML fixtures, assert `WriteToml(ReadSceneBinary(WriteSceneBinary(d))) == WriteToml(d)`. Commit.

## Phase 2: Runtime binary-first load

**Files:** `src/app/scene/SceneSerializer.cpp`

- `ReadSceneFile` / `ReadPrefabFile`: try the `.scene.bin` / `.prefab.bin` sibling first (VFS `project://` then disk); on success `ReadSceneBinary`; else fall back to the existing TOML path.
- Read binary bytes via `io::FileSystem::ReadFileBytes` / `io::file_util::ReadBytes`.
- The prefab cache from the previous work sits above this, so a cached prefab skips both.

**Verify:** editor loads a scene that has both `.bin` and `.toml`, and one with only `.toml`. Commit.

## Phase 3: Editor cooks the binary sibling on save

**Files:** `src/app/scene/SceneSerializer.cpp`, `.gitignore`

- `SaveSceneFile` / `SavePrefabFile`: after writing the `.toml`, also write the cooked `.bin` sibling (same directory, VFS + disk). Refresh happens together so they never drift.
- Gitignore `*.scene.bin` and `*.prefab.bin` (derived artifacts).
- These siblings live in the project dir, so the existing project.pak packing picks them up and the shipped runtime loads binary.

**Verify:** save a scene in the editor → `.scene.bin` appears; reload → binary path is taken (add a debug log or check via size). Commit.

## Phase 4: End-to-end verification

- Drive the editor via MCP: `new_scene`, populate components, `save_scene`, confirm the `.bin` exists and `load_scene` round-trips (entity/component counts match).
- Compare `.bin` vs `.toml` byte size on a representative scene and note the parse-time win.
- Full test suite green. Commit.
