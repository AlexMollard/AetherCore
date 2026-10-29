# Asset database and persistent identity

Project assets have a persistent 128-bit identity independent of their path. Runtime
`AssetId` handles remain opaque 64-bit catalog keys; indexed file sources derive those
keys from persistent identity rather than the filename. Built-in primitives keep their
deterministic engine identities.

## Persistent project data

- `<asset-file>.uid` stores the asset's generated identity. Keep the sidecar with its
  asset in source control and external file moves. Reimporting the file does not replace
  its sidecar.
- `asset-catalog.toml` has its own schema version and maps identities to current
  project-relative paths. It also records document/field reference locations.
- Authored references use `uid://<32 lowercase hexadecimal digits>`. An unknown UID
  is an error, not permission to load a different file at an old path.
- The dependency/reference index is derived data. Queries rescan current authored
  documents, including binary tilemaps, rather than trusting stale reference counts.
- Runtime packages contain the catalog but exclude sidecars and migration backups.
  Native loose files, native packages, and the browser use the same UID representation.

`AssetCatalog` owns discovery, migration, reference queries, and move transactions.
`FileSystem::AssetReference`, `ResolveAssetPath`, and `ReloadAssetCatalog` connect that
catalog to resource loading. Consumers that derive cooked filenames resolve the UID
before changing extensions; authored records retain identity for subsequent saves.

## Migrating an existing project

Normal project loading does not create sidecars or rewrite legacy files. Migration is
explicit; preview first, then apply:

```powershell
$packer = '.\build\default\tools\RelWithDebInfo\AssetPacker.exe'
& $packer migrate-assets 'projects\MyGame'
& $packer migrate-assets 'projects\MyGame' --apply
```

Apply validates supported schemas and references, upgrades model bakes where needed,
stages changes, backs up replaced files under `.asset-migration-backups/<transaction>/`,
and commits the staged files. Failed commits roll back and report any rollback failure.
TOML migration preserves values, not original comments or formatting; backups retain
the original bytes. Generated metadata and text use CRLF.

Legacy path references remain supported in unmigrated projects. Once migrated, saves
preserve GUID references. Run migration again to register newly added assets or rebuild
the packaged catalog after external moves. Duplicate live sidecar identities are
rejected instead of selecting one arbitrarily.

## Safe rename and move

```powershell
& $packer move-asset 'projects\MyGame' 'assets/textures/hero.png' 'assets/textures/player.png'
& $packer move-asset 'projects\MyGame' 'assets/textures' 'assets/art'
& $packer asset-references 'projects\MyGame' 'project://assets/art/player.png'
```

The destination is the complete project-relative target path, not merely its parent
directory. Its parent directory must already exist. The File Explorer uses the same
catalog transaction for inline rename and drag/drop moves; unmigrated assets cannot
be moved through that path.

Moves preflight containment, collisions, sidecars, and dependent references. They move
identity metadata with the asset and update the catalog without rewriting GUID-bearing
scene/prefab references. Unsafe relative glTF dependency moves are rejected; moving a
self-contained folder preserves those relative relationships. Font source moves include
their generated `.fontcurves` companion. Live world paths, texture registry entries,
catalog entries, and the active scene reference are retargeted without changing handles.

The index covers authored resource fields; it is not a source-code refactoring engine.
Arbitrary script strings and dynamically constructed file paths are not rewritten.
Use UID references when authoring runtime asset loads that must survive moves.

## Scene, prefab, component, and asset upgrades

| Boundary | Current version | Compatibility |
|---|---:|---|
| Scene TOML | 17 | Legacy scene migrations retained; future/malformed versions rejected |
| Prefab TOML | 1 | Independent `[prefab].version`; legacy headerless prefabs accepted |
| Component tables | 1 | `schema_version`; missing legacy versions accepted |
| Sprite atlas | 3 | Earlier supported schemas accepted |
| Sprite animation | 2 | Legacy v1 accepted |
| Tileset | 2 | Legacy v1 accepted |
| Binary tilemap | 2 | Legacy v1 accepted |
| Authored material | 1 | Missing legacy version accepted |
| Cooked mesh | 4 | Explicit v3 decoder retained |
| Asset catalog | 1 | Unsupported catalog schemas rejected |

Cooked scenes retain their independent binary envelope version. Skeleton, animation,
animation-set, material, font, and package readers validate their own format headers;
package pipeline version 8 invalidates old caches for stable mesh keys and unique outputs.
Migration refuses newer schemas rather than silently downgrading them.

Cooked v4 meshes carry logical submesh keys from identifiable source node/mesh/material
names. Keys survive geometry edits and named submesh reordering. Removed, duplicate,
or unidentifiable keys fail closed rather than binding another primitive. Explicit
migration materializes legacy model references from an identifiable v4 bake. Ambiguous
source models require disambiguating source names before migration can complete.

Packages contain one output per virtual path. Authored material overrides take
precedence over fresh source cooks, which take precedence over baked caches.
Conflicting outputs at the same precedence fail without replacing the existing
package, rather than silently selecting a different resource.

## Runtime catalog and mesh resolution

`AssetDatabase` catalogs meshes, models, textures, materials, and 2D assets without
owning a second GPU cache. Texture and material resolution continues through the
existing registries. Built-in meshes resolve through `PrimitiveMeshes`; project meshes
use the app-injected model resolver.

`MeshComponent.asset` is the runtime handle, while its mesh pointer is a resolved
cache. `ResolveWorldMeshes` uses catalog generations to refresh that cache after
`Touch`/`TouchByPath`. Model reload retains old cache slots until scene teardown so
existing render pointers do not dangle.

The mesh resolve pass does not replace a skinned model's animation database after
reload. That existing lifecycle limitation is separate from persisted asset identity.

## Streaming residency accounting

`streaming::BudgetLedger` admits CPU and GPU content reservations independently.
Pending, resident, and retiring allocations all consume their domain's budget.
Staging consumes both the CPU budget and its own staging limit.

Admission and transfers are thread-safe and overflow-checked. Rejected reservations
leave existing charges unchanged. Lowering a budget does not erase live charges:
same-domain transitions into retirement remain possible, and new allocations wait
until enough charged memory has actually been released.

Callers reserve before allocating and release only when the corresponding ownership
ends. These are streaming-content limits, not a whole-process heap or driver-memory
ceiling; renderer baseline allocations and driver overhead are separate.

Cross-thread loader tasks serialize completion with waiter registration and retain
their result until ownership ends. `CoroutineTaskTests` checks exact-once result
destruction and completion between the awaiter's readiness and suspension steps.

## Regression coverage

`AssetCatalogTests`, `AssetReferenceLifecycleTests`, `ModelIdentityTests`, and
`SchemaVersionTests` exercise read-only previews, backups, unchanged references across
file/folder moves, restart/reimport identity, moved scene/prefab links, binary tilemap
package loading, named model reimports, missing UIDs, and unsupported schemas.
