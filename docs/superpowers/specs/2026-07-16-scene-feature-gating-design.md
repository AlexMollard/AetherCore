# Scene Feature Gating: 2D/3D Domain Separation

**Date:** 2026-07-16
**Status:** Approved (full-sweep scope; Mixed scenes get 3D physics only)

## Problem

`SceneKind` and `SceneFeatureFlags` exist and persist per scene (format v10+), but only the renderer consumes them. Every 2D/3D behavioural split so far became an ad-hoc gate (per-system scene-kind checks, hand-written editor conditions), and Physics2D was about to add three more. Nothing prevents an entity from simulating in both Jolt and Box2D simultaneously, and nothing stops 2D physics from running in a 3D project.

## Design

### 1. Feature model and policy

- Add `SceneFeatureFlags::Physics3D` (1u << 6).
- `AllowedSceneFeatures(SceneKind)` beside `DefaultSceneFeatures`: everything except `Physics3D` for `Scene2D`; everything except `Physics2D` for `Scene3D`/`Mixed`. Physics is the only hard exclusion — meshes-in-2D and sprites-in-3D stay legal per the 2D roadmap.
- `DefaultSceneFeatures`: Scene3D/Mixed gain `Physics3D`; Scene2D gains `Physics2D` (amended from first-use-only so menus can filter on ACTIVE features).
- Scene format bumps v13 -> v14 with a migration granting `Physics3D` to existing 3D/Mixed scenes and `Physics2D` to existing 2D scenes.
- MCP `add_component` still first-use-enables an allowed-but-off feature; serializer apply reconciles features implied by file content.

### 2. System activation

- `System` gains `virtual SceneFeatureFlags RequiredFeatures() const` (default `None` = always active) and non-virtual `IsActiveIn(const World&) const`.
- `SystemRegistry::UpdateAll` skips systems whose required features are not all active.
- Mapping: PhysicsSystem -> Physics3D; Physics2DSystem -> Physics2D; SpriteAnimationSystem -> Sprites; LightSystem, DayNightSystem -> Lighting3D; AnimationSystem -> Meshes3D; Camera/Behavior/Script systems -> None.
- `Application.cpp` edit-mode preview/flush calls gate on `IsActiveIn` instead of running unconditionally.
- The scene-kind checks recently added inside Physics2DSystem come out; activation is the registry's job.

### 3. Authoring enforcement

- `reflect::ComponentType` and `editor::ComponentCatalogEntry` gain `SceneFeatureFlags requiredFeatures` and `std::vector<std::string> conflictsWith` (catalog-entry names).
- One helper, `editor::ComponentAddBlockReason(world, entity, entry)`, returns empty-or-reason:
  - required feature not in `AllowedSceneFeatures(kind)` -> "requires <feature>, which a <kind> scene cannot enable";
  - any conflicting entry's `has(world, entity)` true -> "conflicts with <name>".
- Consumers (amended per review): editor menus HIDE entries for inactive domains instead of disabling them - `ComponentVisibleInMenu` (features ACTIVE + no conflicts) drives the Inspector Add-Component palette (which gains the missing "Physics 2D" section) and the Hierarchy create menu (2D submenu <- Sprites; primitives/perspective camera <- Meshes3D; lights <- Lighting3D). MCP `add_component` keeps the explicit `ComponentAddBlockReason` error and first-use feature enabling.
- 2D physics components conflict with {Rigid Body, Box Collider, Joint}; 3D physics entries conflict with {Rigid Body 2D, Collider 2D, Joint 2D}.

### 4. Serializer and defense in depth

- Apply-time reconcile: features implied by applied records (sprites, meshes, lights, both physics domains) are OR'd into the scene features when allowed; physics records whose domain is disallowed for the scene kind are skipped with a warning.
- Per-entity conflicts in a file keep the domain matching the scene kind and warn.
- Both physics system flushes skip entities carrying the other domain's components (hand-edited files; warn).
- `PhysicsDomainGate` slims to the two entity-level checks (`EntityHas2DPhysics` / `EntityHas3DPhysics`); scene-level policy lives in `AllowedSceneFeatures`.

### 5. Testing

- Registry skips a feature-gated system whose flag is off (Physics2D system inert in a Scene3D world).
- Existing Physics2D suite runs in `Scene2D` worlds with the flag set — proving the gate.
- v14 migration fixture: v13 3D scene with physics records loads with `Physics3D` granted.
- Cross-domain entity: 2D flush skips and warns; serializer keeps the kind-matching domain.
- Editor/MCP runtime check: 2D component add refused in a 3D scene with a readable reason.

## Rejected alternatives

- **Separate system sets per scene kind:** runtime scene loads change kind, forcing re-registration churn; can't express per-feature choices like sprites-in-3D.
- **Per-entity domain tag components:** bookkeeping on every entity; solves nothing at scene level.
