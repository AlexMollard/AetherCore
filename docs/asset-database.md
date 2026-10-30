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
| Scene TOML | 18 | Adds authored streaming regions; legacy scene migrations retained; future/malformed versions rejected |
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
`Touch`/`TouchByPath`. Legacy synchronous model reload retains old cache slots until
scene teardown. Streamed models instead retain strong packet leases and defer GPU
destruction until the consuming submissions have retired.

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

## Authored world regions

A root scene can reference independent chunk scenes without instantiating all of them.
Scene version 18 retains the region definitions when the root is saved:

```toml
[[stream_regions]]
id = "village"
scene = "project://scenes/Village.scene.toml"
bounds_min = [-128.0, -32.0, -128.0]
bounds_max = [128.0, 96.0, 128.0]
unload_margin = 16.0
priority = 1
always_loaded = false
```

Migrate the project to replace the path with its persistent UID. The reference index
includes `stream_regions.scene`, so safe chunk-scene moves preserve the root link.
Region IDs and authored entity node IDs identify snapshot records; changing them
changes the identity of the corresponding region or entity.

The default main-camera source loads nearby regions and prefetches a larger radius.
Additional sources and explicit requests share the same controller. Priority values
are Critical (0), High (1), Normal (2), and Background (3); the strongest applicable
demand wins. `unload_margin` provides hysteresis when a source leaves a region.

Prefetch makes the dependency closure available without activating region entities.
An active request waits for dependencies and GPU uploads, then applies records over
the shared per-frame activation allowance. Partially applied entities remain disabled.
Cancellation invalidates pending work and prevents late results from activating it.
Every explicit request must eventually be released or cancelled.

### Budgets and observability

Project overrides use the existing settings cascade:

```toml
[streaming]
cpuBudgetMiB = 256
gpuBudgetMiB = 512
stagingBudgetMiB = 32
uploadBudgetKiBPerFrame = 4096
maxPendingRequests = 128
activationEntitiesPerFrame = 64
loadRadius = 128.0
prefetchRadius = 256.0
defaultCameraSource = true
```

Budgets are admission limits. A request that cannot fit reports `Blocked`; raising
the relevant limit or releasing other claims permits progress. Shrinking a limit
does not destroy currently referenced resources or forgive existing charges.
Staging and unfinished uploads remain charged until completion or safe retirement.
GPU uploads are split across frames; activation has a separate entity allowance.

CPU figures include conservative decode, prepared-document and snapshot reservations,
not just measured resident bytes. Pending workspace reservations can remain charged
while their prepared payload is retained. Native GPU figures use allocation
requirements; browser GPU figures use logical resource sizes. Worker runtime heaps,
renderer baseline resources, allocator slack and driver overhead are not a
whole-process memory guarantee. A region's content must fit the configured admission
limits; moving content into a chunk does not make an oversized individual resource
streamable.

The `streaming.status` control method and gameplay diagnostics report region demand,
state, priority and errors, asset claims, pending/resident/retiring bytes, staging and
per-frame upload bytes. Shared asset rows are not additive. Unloading a region releases
its claims, not another region's shared dependencies. Render packets retain CPU owners;
GPU charges end on real completion/destruction, not simply when an entity disappears.

### Runtime state and gameplay

Normal region unload captures mutable component data, script public properties and
custom script state, then removes its ECS, physics and script instances. Unloaded
regions do not continue simulating. Snapshots are session-private storage, not a
save-game format or an engine-upgrade persistence contract.

The capture point precedes script detach cleanup. Re-entry restores that state on new
runtime entity handles. Entity/component references use stable nodes, including
deferred cross-region references; do not serialize raw runtime handles in custom
state. Persistent actors leave regional ownership and retain their own dependency
claims. Whole-scene replacement resets the old streaming session.

Streamed nodes retain both a region-qualified runtime identity and their authored
`source_node` identity. References bind exact runtime identities first; an authored
identity binds only when it names one live instance. Once bound, a reference retains
the qualified identity, so another instance of the same chunk cannot steal it.
Missing external targets remain null and rebind when the target returns, including
when the referencing region itself was snapshotted while the target was absent.

Snapshot writes are asynchronous. Until durable completion, the rollback payload and
its resource claims stay alive. A failed save restores the retained state instead of
silently discarding it. Script callbacks can have external side effects; those are
not a global transaction rolled back by the streaming system.
Native snapshot admission reports `Blocked` while CPU/staging capacity is unavailable,
retains the live region, and retries when capacity becomes available. Concurrent
unloads therefore wait rather than discard state or permanently fail on staging
contention. A new demand transition can retry a browser region after save rollback.

The [C# Streaming API](../managed/AetherCore/Streaming.cs) provides `SetSource`,
`Load`, `Prefetch`, `Cancel`, `Release`, `GetState` and `GetDiagnostics`.
`EntityScript.CaptureStreamingState` / `RestoreStreamingState` preserve private state.
The safe [Rust SDK](../rust/aethercore/src/lib.rs) exposes corresponding
`Context::streaming_*` methods and `Script` capture/restore hooks.
Gameplay ABI major 2 requires rebuilding existing gameplay modules; incompatible
modules fail with a rebuild diagnostic rather than using the old callback layout.

Existing scenes without regions retain their whole-scene loading behavior. Put
large content inside regions to make it eligible for streaming ownership and budgets;
ordinary root-scene and renderer baseline content is not automatically partitioned.

## Regression coverage

`AssetCatalogTests`, `AssetReferenceLifecycleTests`, `ModelIdentityTests`, and
`SchemaVersionTests` exercise read-only previews, backups, unchanged references across
file/folder moves, restart/reimport identity, moved scene/prefab links, binary tilemap
package loading, named model reimports, missing UIDs, and unsupported schemas.

`WorldStreamingTests` covers independent source hysteresis, claim priority and
cancellation, bounded admission, world reset, malformed binary documents, ambiguous
authored aliases, canonical references across repeated instances, nested reference
values, and script entity/component references after re-entry.
`AssetReferenceLifecycleTests` also exercises denied decode reservations and retry.
`StreamingBudgetTests` covers staging, retirement, budget shrinking and overflow-safe
transfers. Browser package validation accepts
cooked content from either the bootstrap index or integrity-checked streaming blobs;
missing, inconsistent or escaping blob declarations fail closed.
