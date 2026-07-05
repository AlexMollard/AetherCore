# Scene Serialization — Design

- **Date:** 2026-07-05
- **Status:** Approved
- **Spec:** 3 of 3 (follows Viewport Picking & Gizmo)
- **Owner:** Alex Mollard

## 1. Motivation

Specs 1–2 built the full editor loop (outliner ↔ inspector ↔ viewport pick/gizmo), but arrangements die with the process: the world is spawned by `sandbox.das on_attach` and nothing persists hand edits. This spec adds **save/load of entity + component state**, including the stable asset-reference story that pointer-bearing components (`MeshComponent.mesh`, `PipelineComponent.pipeline`, `SkinnedMeshComponent.animDb`, `MaterialAsset` texture handles) require.

## 2. Decisions

- **Files**: real TOML (`[[entities]]` arrays-of-tables) written directly with the vendored tomlplusplus (`TomlConfig` stays a flat settings wrapper). Location: **repo `resources/scenes/<name>.scene.toml`** in dev builds via an `AETHER_SCENES_SOURCE_DIR` compile-def (mirroring script hot-reload); `EngineSettingsIO::ResolvePath` fallback otherwise. Scenes are committable content.
- **Load = replace**: destroying every live entity (all on_destroy hooks fire: Jolt bodies, material slots, effect slots) then instantiating the file. Loaded entities register into `SceneContext.sceneEntities` so **F5 still returns to the pristine script scene**. Lights, cameras and day/night are script/renderer state, not entities — untouched by save/load, so script lighting composes with loaded arrangements.
- **Known limitations (accepted)**: script-held entity ids (`g_player`, orb handles…) go stale after Load until F5 re-runs the script; dynamic bodies restore at rest (no velocity); externally-added animation clips exist only if the model's cached `AnimationDatabase` already compiled them this session (clipIndex is clamped otherwise).

## 3. Stable identity

| Pointer/handle | Stable identity | How |
|---|---|---|
| `MeshComponent.mesh` | model path + primitive index, or primitive kind | **new `MeshSourceComponent`** written at spawn (das `load_model`/`add_mesh`, outliner CreatePrimitive) — no reverse map exists |
| `MaterialAsset.*Tex` handles | VFS path | **new `TextureRegistry::TryGetPath(handle)`** over the entry's stored `resolvedPath` |
| Material itself | `MaterialAsset` values | `MaterialInstanceComponent.asset`, else `MaterialRegistry::TryDescribe(handle)` (Spec 1) |
| `PipelineComponent.pipeline` | derived | never serialized — rebuilt by `AssignMaterial` or effect re-apply |
| Effect pipeline + slot | effect name | **new `EffectRefComponent{name}`** written by `set_entity_effect` |
| `SkinnedMeshComponent.animDb` | the model source | rebuilt from `LoadedModel.animationDb` + `primitives[i].skinIndex`; state = clip/time/speed/looping |
| Physics body | shape + motion | `PhysicsDebugShapeComponent` dims + `RigidBodyComponent.motionType` → re-emplace `*BodyDesc`; `FlushPendingBodies` rebuilds the Jolt body (das never exposed friction/layer, defaults are faithful) |
| Tags | names | `ForEachTag` + `TagHas` → `TagCreate`/`TagAdd` |

## 4. Serializer architecture

Two stages for testability:
- `CaptureScene(World&, deps) → SceneDescription` — pure records (name, tags, TRS via `DecomposeTRS`, parent as file-local index, mesh source, material asset + texture paths, skinned state, physics shape+motion, effect name+params).
- `WriteToml/ParseToml` ↔ `SceneDescription` (toml++), `SaveSceneFile/ListSceneFiles/LoadSceneFile` on top.
- `ApplyScene(SceneDescription, World&, deps)` — create all entities; per entity emplace value components + physics descs, resolve assets (model cache → `AssetManager::LoadModel` on miss; `PrimitiveMeshes::Get`; texture `Acquire` by path → `AssignMaterial`; `ApplyEntityEffect` by name with saved params — extracted from the das binding); hierarchy `SetParent` fix-up pass last.

`SceneContext` becomes a registered service (it owns the model cache, effect manager pointer and `sceneEntities`), so the debug-UI serializer reaches everything without being inside a script call.

doctest covers record↔TOML round-trips and the non-GPU apply path (real `World`, fake material sinks); GPU-visible restoration is the RUN checklist.

## 5. UI

Outliner toolbar gains save/folder icons: Save popup (name field, writes `<name>.scene.toml`, logs the path), Load popup (lists the scenes dir, Load per row). Load clears the selection.

## 6. Follow-ons

Boot-from-scene-file; scene deltas/prefabs; undo/redo spec (hooks `ApplyWorldTransform` + this serializer's records); saving lights/cameras once they become entities or gain their own registry.
