# Asset Database

A central, GUID-handle asset system so components reference assets by a **stable
id** instead of raw resource pointers or ad-hoc path strings. It hardens
serialization, decouples the renderer from fragile cached pointers, and is the
seam for future hot-reload, streaming, and rename-stable `.meta` GUIDs.

## Why

Before this, a drawn mesh was a raw `const Mesh*` on `MeshComponent`, pointing
either into the engine-owned `PrimitiveMeshes` cache or into a `LoadedModel`
owned by the app-layer `SceneContext`. That pointer is fragile: it is invalid
across a reload, cannot be serialized, and every consumer had to re-resolve a
`MeshSourceComponent{kind, path, primitiveIndex}` by hand (the serializer, the
inspector, the script API each duplicated the mapping).

The asset database turns "where a resource comes from" into a first-class,
hashable identity and gives one place to resolve, enumerate, and reload it.

## Core types (`src/engine/assets/`)

- **`AssetId`** (`AssetId.hpp`) — an opaque 64-bit id. `0` is the invalid
  sentinel. Serializes as 16-char hex (`ToHex`/`FromHex`). Has a `std::hash`
  specialization so it keys unordered maps directly.
- **`AssetSource`** (`AssetTypes.hpp`) — the canonical descriptor of where an
  asset comes from: `{ AssetType type; std::string path; int subIndex; bool
  builtin; }`. `ComputeAssetId(source)` is a deterministic FNV-1a hash of it, so
  the same source always yields the same id across sessions.
  - built-in primitive mesh → `MakePrimitiveMeshSource("cube")` (`builtin=true`)
  - glTF model primitive → `MakeModelMeshSource("project://foo.glb", 2)`
  - whole model / texture → `MakeModelSource` / `MakeTextureSource`
- **`AssetDatabase`** (`AssetDatabase.hpp/.cpp`) — the catalog service:
  - `Register(source, name) -> AssetId` (idempotent interning)
  - `Describe` / `TypeOf` / `DisplayName` / `Generation` / `Contains`
  - `Touch(id)` bumps a per-asset generation — the hot-reload signal
  - `ResolveMesh(id) -> const Mesh*`
  - `ForEach(type, fn)` for pickers, in registration order

## Layering: engine core, app-injected model resolution

The database lives engine-side and must not depend on the app layer, but the
glTF model cache (`loadedModels`) lives in the app-layer `SceneContext`. So:

- Built-in **primitive** meshes resolve natively via `PrimitiveMeshes`.
- **Model primitive** meshes resolve through an injected callback,
  `AssetDatabase::SetModelMeshResolver(...)`, wired in
  `ScriptedSceneLayer::OnAttach` to `scene::ResolveModelPrimitiveMesh` (which
  loads through the `SceneContext` cache — the same shared pointer scene load
  resolves).

```
MeshComponent.mesh (live ptr)  ← resolved from ←  AssetId  ← hash of ←  AssetSource
                                                     │
                                    AssetDatabase ───┤ primitive → PrimitiveMeshes
                                                     └ model     → app resolver → SceneContext cache
```

## Lifecycle & integration

- **Bootstrap** (`AssetSubsystem::Init`): the database is constructed with
  `PrimitiveMeshes`, registered as a service, and seeded with the five built-in
  primitives (`RegisterBuiltinPrimitives`).
- **Model load/assign** (`ModelSpawn::RegisterModelAssets`): dropping or
  assigning a model registers the model and each of its primitives, so they
  appear in the picker. Called from the inspector and hierarchy drop paths.
- **Scene load** (`SceneSerializer::ApplyScene`): every resolved mesh source is
  registered, so the catalog reflects a loaded scene immediately.
- **Inspector** (Mesh Renderer panel): the "Mesh" field is a handle-based asset
  picker — click it to choose any mesh asset (primitives + loaded model meshes)
  from the database; selection resolves and rebinds the entity's mesh.

## Textures & materials: one catalog, existing registries as backends

Textures and material presets are catalogued too (`AssetType::Texture` /
`AssetType::Material`), but the database does **not** re-own them — it resolves
*through* the existing systems, so there is no second cache or ref-count:

- A texture `AssetId` → `TextureRegistry::Acquire(path)` → `TextureHandle`.
- A material-preset `AssetId` → `AssetManager::LoadMaterialPreset(path)`.

They are catalogued **on use** (dropped from the File Explorer, or resolved
during scene load), Unity-style. The inspector's mesh field, sprite texture
slot, and each material texture map share one `AssetPickerButton` that reads the
catalog; the Material section also has a preset picker.

The rule: **if an asset already has a registry/handle, its `AssetId` resolves to
that handle rather than creating a second identity.** Only meshes — which had
only a raw `const Mesh*` — are owned by the catalog.

## Serialization

Entity references still persist by source (`MeshSourceComponent`, material
texture paths, `UIImage.texturePath`) — stable and backward-compatible.

In addition, `CaptureScene` writes an **`[[assets]]` manifest** mapping every
referenced mesh/texture to its stable `AssetId` + source, and `ApplyScene` seeds
the catalog from it (so a loaded scene's assets appear in the picker
immediately). The section is additive: it's omitted when empty, and scenes that
predate it load unaffected. This puts the ids in the file and is the seam for
rename-stable references.

## Id-primary meshes + the resolve pass

`MeshComponent` is now **id-primary**: `asset` (an `AssetId`) is the
authoritative reference, and `mesh` (the `const Mesh*` the renderer reads) is a
resolved cache. `AssetDatabase::ResolveWorldMeshes(world)` — called once per
frame from `Application::OnUpdate`, in both play and edit modes — keeps them in
sync:

1. back-fills `asset` from `MeshSourceComponent` the first time it sees a mesh;
2. re-points `mesh` whenever the asset's **generation** has bumped since the
   last resolve (steady state is a single generation compare per mesh).

In the no-reload case this is a no-op (the first resolve returns the same
pointer already set at spawn), so normal rendering is untouched.

## Hot-reload

`AssetDatabase::Touch(id)` / `TouchByPath(path)` bump generations; the resolve
pass re-points meshes on the next frame. `scene::ReloadModelAssets` re-reads a
model file into a **fresh** cache slot (a `std::deque`, so old element
references stay valid) and touches its primitives — live `mesh` pointers into
the old slot never dangle; they are re-pointed to the new slot next frame, and
the old slot is orphaned until scene teardown (leak, not crash). The Mesh
Renderer has a **Reload from disk** button for model meshes.

Limitation: the resolve pass re-points *meshes* only. A skinned model's
`SkinnedMeshComponent.animDb` still points at the old slot after a reload (valid,
so no crash; only a mismatch if the skeleton structure changed) — re-pointing
the animation database is a follow-up.

## Roadmap (not yet implemented)

- **`.meta` GUIDs**: swap the content-hash id for a random per-asset GUID
  (persisted in a project manifest) so an edit to a source doesn't change its id
  and a rename only updates the manifest. Because references are already
  id-primary and opaque, this is a database-internal change — no component or
  scene-format churn.
- **Id-primary textures/materials**: same treatment for texture references
  (`UIImage.texturePath`, material maps), resolving through `TextureRegistry`.
- **OS file-watch**: watch source files and call `TouchByPath` automatically,
  instead of the manual Reload button.
- **Async / streaming**: an id can be "loading"; resolve returns a placeholder
  until the real resource arrives.
