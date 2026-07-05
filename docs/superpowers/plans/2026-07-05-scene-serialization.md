# Scene Serialization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans, task-by-task with checkboxes. Spec: `docs/superpowers/specs/2026-07-05-scene-serialization-design.md`.

**Goal:** Save/load the entity world to `resources/scenes/<name>.scene.toml` — provenance components give pointer-bearing components stable identity; Load replaces all entities and keeps F5 script-reset intact.

**Verification model:** BUILD = `cmake --build build-vs2022-msvc --config Debug --target App EngineTests` (single `--target` list); TEST = run `build-vs2022-msvc/tests/Debug/EngineTests.exe`; reconfigure after adding files; RUN = hand-off checklist for Alex. One commit per task, footer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

## Task U1: Identity foundations

**Files:** modify `src/engine/material/TextureRegistry.hpp/.cpp`, `src/engine/scene/Components.hpp`, `src/app/scripting/modules/WorldModule.cpp`, `src/app/scripting/SceneContext.hpp`, `src/app/scripting/modules/EffectsModule.cpp`, `src/app/debug/HierarchyPanel.cpp`; create `tests/material/TextureRegistryPathTests.cpp` (or extend existing); modify `tests/CMakeLists.txt` if a new file.

- [x] `bool TextureRegistry::TryGetPath(TextureHandle, std::string& out) const` — validate index/generation/alive, copy `m_entries[i].resolvedPath`.
- [x] `MeshSourceComponent { enum class Kind { Model, Primitive }; Kind kind; std::string path; std::uint32_t primitiveIndex = 0; }` + `EffectRefComponent { std::string name; }` in Components.hpp.
- [x] Writers: `das_load_model` emplaces `{Model, path, i}` per mesh child (loop order == primitive order); `CachedMesh` gains `kindName`; `das_add_mesh` emplaces `{Primitive, kindName, 0}`; `das_set_entity_effect` emplaces `EffectRefComponent{name}`; `HierarchyPanel::CreatePrimitive` emplaces `{Primitive, kind string, 0}`.
- [x] doctest: Acquire("brick.png") → TryGetPath returns the resolved path; stale/invalid handle → false.
- [x] Reconfigure if new test file + BUILD + TEST. Commit `feat(scene): mesh/effect provenance components + texture path lookup`.

## Task U2: SceneContext service + shared effect apply

**Files:** modify `src/app/layers/ScriptedSceneLayer.cpp`, `src/app/effects/EffectManager.hpp/.cpp` (or new ApplyEffect files), `src/app/scripting/modules/EffectsModule.cpp`.

- [x] `ScriptedSceneLayer::OnAttach` → `context.services.Register<scripting::SceneContext>(m_sceneCtx)`; `Unregister` in OnDetach.
- [x] Extract `ApplyEntityEffect(World&, Entity, std::string_view name, EffectManager&, PipelineCache&, EffectParamBuffer&, const EffectParams* overrideParams)` from `das_set_entity_effect` (das binding calls it; also emplaces `EffectRefComponent`).
- [x] BUILD. Commit `refactor(effects): shared ApplyEntityEffect + SceneContext service`.

## Task U3: SceneSerializer capture + TOML I/O

**Files:** create `src/app/scene/SceneSerializer.hpp/.cpp`, `tests/scene/SceneSerializerTests.cpp`; modify `tests/CMakeLists.txt`, `src/app/CMakeLists.txt` or root CMake for `AETHER_SCENES_SOURCE_DIR` (mirror `AETHER_SCRIPTS_SOURCE_DIR`).

- [x] `SceneDescription { struct Entity { name, tags[], pos/euler/scale, parentIndex(-1), optional MeshSource, optional MaterialRecord (asset values + 5 texture paths), optional SkinnedState, optional PhysicsRecord (shape, dims, motion), optional EffectRecord (name, params) }; std::vector<Entity> entities; }`.
- [x] `CaptureScene(World&, const MaterialRegistry&, const TextureRegistry&) → SceneDescription` — registry walk in storage order; parent = index into captured list (entities whose parent is not captured → root); material from `MaterialInstanceComponent.asset` else `TryDescribe`; texture handles → `TryGetPath`.
- [x] `WriteToml(const SceneDescription&) → std::string` and `ParseToml(std::string_view) → std::optional<SceneDescription>` via toml++.
- [x] Scenes dir: `AETHER_SCENES_SOURCE_DIR` compile-def (dev) else `EngineSettingsIO::ResolvePath("scenes")`; `SaveSceneFile(name, desc)`, `ListSceneFiles()`, `ReadSceneFile(name)` (std::ofstream/ifstream + std::filesystem).
- [x] doctest: build a fake-backed world (names, tags, transforms, hierarchy, physics debug-shape + rigid stub, material instance with textures) → Capture → Write → Parse → deep-compare records.
- [x] Reconfigure + BUILD + TEST. Commit `feat(scene): scene capture + TOML serialization + scenes directory`.

## Task U4: ApplyScene (load, replace-all)

**Files:** modify `src/app/scene/SceneSerializer.hpp/.cpp`, `tests/scene/SceneSerializerTests.cpp`.

- [x] `ApplyScene(const SceneDescription&, World&, ApplySceneDeps{AssetManager*, PrimitiveMeshes*, MaterialRegistry*, PipelineCache*, TextureRegistry*, EffectManager*, EffectParamBuffer*, scripting::SceneContext*})`:
  1. create all entities (record file-index → Entity),
  2. per entity: Name, Tags (TagCreate+TagAdd), Transform (`ComposeTransform`), physics `*BodyDesc` emplace (from shape record + motion), mesh resolve (Model: SceneContext model cache / `AssetManager::LoadModel` on miss → `primitives[i].mesh`; Primitive: `PrimitiveMeshes::Get`) + re-emplace `MeshSourceComponent`, material (`TextureRegistry::Acquire` per path → `MaterialSystem::AssignMaterial`), skinned rebuild (animDb + skinIndex from primitive + `GetSkinJointCount`, clamp clipIndex, apply saved state), effect via `ApplyEntityEffect` (+ `EffectRefComponent`),
  3. hierarchy pass: `ecs::SetParent` per record,
  4. register all into `SceneContext.sceneEntities`.
- [x] `LoadSceneFile(name, World&, deps)` = parse → destroy ALL valid registry entities (`world.Destroy` walk; hooks release bodies/slots) → clear+refill `sceneEntities` → ApplyScene.
- [x] doctest (non-GPU): round-trip a world through Capture→Toml→Parse→Apply into a FRESH world (fake material sinks, no AssetManager/effects deps) — names, tags, transforms, hierarchy links, physics descs match.
- [x] BUILD + TEST. Commit `feat(scene): ApplyScene load path with replace-all semantics`.

## Task U5: Scene UI

**Files:** modify `src/app/debug/HierarchyPanel.hpp/.cpp`.

- [x] Toolbar: save icon → popup (name InputText seeded "scene", Save button → `SaveSceneFile(CaptureScene(...))`, logs path); folder icon → popup (`ListSceneFiles()` rows with Load buttons → `LoadSceneFile`, then `selection.Clear()`).
- [x] BUILD. Commit `feat(debug): scene save/load UI in the outliner toolbar`.

## Task U6: Acceptance

- [x] Tick non-RUN checkboxes; memory notes; deviations recorded in spec if any (one: U3 and U4 landed as a single serializer commit — same file, one implementation; EngineTests compiles the serializer + TagSlots + EffectManager app TUs directly since it links Engine only).
- [ ] **RUN hand-off (Alex):** arrange with gizmo → Save "test" → file in `resources/scenes/test.scene.toml`, human-readable → move/delete things → Load "test" → arrangement restores: fox animating (cached animDb), Zone-M colors intact, plasma/molten orbs tinted + animating, toys recreate and settle on the floor → F5 → pristine script scene → Load again → arranged scene. Also: delete-all via outliner then Load restores; save after material edits round-trips them; script logic touching stale ids after Load fails soft (no crash).
- [x] Commit `docs: tick Spec-3 plan, acceptance notes`.
