# Component Reflection — Design Spec

**Date:** 2026-07-14
**Status:** Draft for review

## Problem

AetherCore has 34 component structs. Every component is hand-wired across four
surfaces that must be kept in sync manually:

| Surface | File | Lines | Per-component work |
|---|---|---|---|
| Add/remove palette | `src/app/editor/ComponentCatalog.cpp` | 267 | `has` / `add` / `remove` |
| MCP field editing | `src/app/editor/ComponentFields.cpp` | 228 | `read` / `write` (only 5 of 34 done) |
| Inspector | `src/app/debug/ComponentDrawers.cpp` | 2,191 | a bespoke drawer |
| Scene serialization | `src/app/scene/SceneSerializer.cpp` | 2,387 | TOML read + write |

Adding one editable field means editing up to four files; adding one component
means four new blocks. The MCP covers only 5 components. The goal: **one
declaration per component drives all four surfaces, covering every component.**

The structs are **not clean aggregates** — they interleave authored fields with
runtime state (`animDb`, `gpuSlot`, `backingCamera`, `PhysicsBodyHandle body`,
cached pointers, `std::vector<GltfAnimation> pendingExternalAnims`). Automatic
aggregate reflection (`boost::pfr`) would expose and serialize this runtime junk.
So reflection must be **explicit per-field opt-in with metadata**.

## Decisions (locked)

1. **Mechanism:** macro DSL now; swap to C++26 static reflection later. The
   architecture isolates the *declaration* layer so the swap touches nothing else.
2. **Scope:** all four surfaces (MCP, catalog, serializer, inspector).
3. **Placement:** central app-layer registry files; engine structs stay pure.
4. **Delivery:** ship the core + DSL + all four consumers, migrate a *pilot batch*
   first to prove the round-trip, then migrate the rest in follow-up batches.

## Architecture

### 1. Reflection core (mechanism-agnostic, GameRuntime-safe)

The data model every consumer reads. No macros, no editor/ImGui deps, so it
compiles into **both** Editor and GameRuntime (the serializer needs it at runtime).

```cpp
namespace aether::reflect {

enum class FieldType {
    Float, Int, UInt, Bool, Vec2, Vec3, Vec4, Color3, Color4, Enum, String, EntityRef
};

struct EnumTable { std::vector<std::pair<std::string,int>> values; }; // name <-> value

struct FieldMeta {
    float min = 0.f, max = 0.f, speed = 0.f;  // 0/0 => unbounded drag
    bool  isAngleDegrees = false;             // stored radians, edited/exposed degrees
    bool  serialize = true;                   // editable-but-not-serialized fields set false
    const EnumTable* enumTable = nullptr;
    std::string tooltip;
};

struct FieldDesc {
    std::string name;
    FieldType   type;
    FieldMeta   meta;
    std::function<void(const void* comp, nlohmann::json& out)> get; // type-erased read
    std::function<void(void* comp, const nlohmann::json& in)> set;  // type-erased write
};

struct ComponentType {
    std::string name;      // "Point Light" — matches catalog + MCP type
    std::string category;  // "Rendering"
    std::string icon;      // ICON_FA_* (a string; no ImGui dep)
    std::vector<FieldDesc> fields;

    // Type-erased ECS ops bound to the concrete component at registration.
    std::function<bool  (const World&, Entity)> has;
    std::function<void* (World&, Entity)>        emplaceDefault; // add
    std::function<void  (World&, Entity)>        remove;
    std::function<void* (World&, Entity)>        tryGetRaw;
    std::function<const void*(const World&, Entity)> tryGetRawConst;

    bool addable = true;       // false = reference-only (e.g. UI Text)
    bool serializable = true;

    // Opt-out hooks (null => generic behaviour):
    std::function<void(World&, Entity)> postSet;                 // e.g. Material -> AssignMaterial
    CustomSerializeFns customSerialize;                          // bespoke TOML (Mesh, Material textures)
};

const std::vector<ComponentType>& ComponentTypes();
const ComponentType* FindComponentType(std::string_view name);

} // namespace aether::reflect
```

Lives in `src/app/scene/reflection/` (app/scene is already linked by GameRuntime
via the serializer). Custom **inspector** draw overrides (ImGui) are registered
separately by the editor (see §5) so the core stays ImGui-free.

### 2. Declaration DSL (the swappable layer)

Member-pointer based, so get/set are generated type-safely — no per-field lambdas.

```cpp
AE_COMPONENT(PointLightComponent, "Point Light", "Rendering", ICON_FA_LIGHTBULB)
    AE_FIELD  (color,       Color3)
    AE_FIELD_R(intensity,   Float, 0, 1000)     // ranged
    AE_FIELD_R(radius,      Float, 0, 500)
    AE_FIELD  (castsShadow, Bool)
AE_COMPONENT_END()
```

- `AE_FIELD(member, Type)` captures `&Component::member`; the `Type` tag selects
  the json/TOML/widget conversion. The field name string defaults to the member
  name (stringized), overridable with `AE_FIELD_N("name", member, Type)`.
- `AE_FIELD_R` / `AE_FIELD_META` attach `FieldMeta`.
- `AE_FIELD_CUSTOM(name, Type, getLambda, setLambda)` for **computed fields**
  (e.g. Transform's position/euler/scale over a `localToWorld` matrix).
- `AE_ENUM(EnumType, A, B, C)` builds an `EnumTable`; `Enum` fields reference it.
- Runtime-only fields are simply omitted.

When C++26 reflection is available, `AE_COMPONENT` blocks are replaced by a
generator that fills the same `ComponentType`/`FieldDesc`. **Consumers do not
change.**

### 3. Type dispatch

A single `FieldType`→conversion table implements, once, for each type:
`toJson(void*) / fromJson(void*, json)` (MCP), `toToml / fromToml` (serializer),
and `drawRow(label, void*, meta)` (inspector, editor-only). Adding a `FieldType`
is one entry here, not per component. Colours are `[r,g,b(,a)]` arrays;
angle-degrees fields convert on the boundary.

### 4. Consumers (what collapses)

- **MCP** (`ControlMethods`): `get_component` / `set_component` / `add_component` /
  `remove_component` / `list_component_types` iterate `ComponentTypes()` — all 34,
  no per-component code. (Retires `ComponentFields.cpp`.)
- **ComponentCatalog**: generated from `has`/`emplaceDefault`/`remove` + icon/
  category. Bundle entries (e.g. "Cube" = Mesh+Material) stay hand-written on top.
- **SceneSerializer**: generic field↔TOML loop per component; `customSerialize`
  overrides for the hard ones. Parent/hierarchy index-remap stays as-is.
- **Inspector**: generic rows by `FieldType`; `customDraw` opt-out for bespoke UI.

### 5. Hard cases (declared, with explicit hooks)

- **Material** — copy-on-write via `MaterialInstanceComponent` + `AssignMaterial`.
  Scalar factors are `AE_FIELD_CUSTOM` over the instance; `postSet` re-commits;
  textures are a `customDraw` widget + `customSerialize`.
- **Transform** — `AE_FIELD_CUSTOM` position/euler/scale over `localToWorld`.
- **Mesh source / Script / Behaviors / Physics colliders** — keep `customDraw` +
  `customSerialize`; expose any plain scalar fields generically.
- **Entity references** (`LookAt.target`, parent) — `EntityRef` field type carries
  the id and plugs into the serializer's existing parent-index remap.

### 6. GameRuntime safety

- Core (`ComponentType`, `FieldDesc`, type dispatch for json/TOML, DSL) — no
  ImGui, no editor deps → compiles into GameRuntime for scene load.
- Inspector row-drawing + `customDraw` registry — editor-only (`debug/`),
  registered by name against the core at editor startup.
- Verified each batch: both `Editor` and `GameRuntime` build and link.

### 7. Migration strategy (incremental, coexisting)

The registry coexists with legacy code. A component is "migrated" when its
hand-written catalog/fields/drawer/serializer blocks are deleted and replaced by
one `AE_COMPONENT` declaration. Each consumer checks the registry first and falls
back to legacy for un-migrated components, so we convert in batches. A
**round-trip serialization test** (load a scene → capture → compare TOML) gates
each batch; `HumanDemo` is the live smoke test.

## Pilot batch (first PR)

Ship the full core + DSL + all four consumers wired, migrating these simple
components (member-pointer fields + Transform's computed fields — proves both
paths and all four surfaces end-to-end):

`PointLightComponent`, `SpotLightComponent`, `CameraComponent`,
`SkinnedMeshComponent`, `TransformComponent`, `SpinComponent`, `BobComponent`,
`OrbitComponent`, `ScalePulseComponent`, `MaterialPulseComponent`,
`RootMotionComponent`.

**Deferred to follow-up batches:** enum + physics (`RigidBody`, `Collider`),
`Material` (postSet + texture widget), `EntityRef` (`LookAt`, parent), and the
custom-widget components (`Script`, `Behaviors`, `Mesh`, `Effects`, UI, sprite).

## Success criteria

- One `AE_COMPONENT` block fully replaces a component's four hand-written blocks.
- Every pilot component is get/set/add/remove-able via MCP, serializes round-trip
  identically, and renders a working inspector section — with no per-component
  code in any of the four consumers.
- `Editor` and `GameRuntime` both build; existing tests + a new round-trip test pass.
- Adding a new editable field = one line in one declaration.

## Non-goals

- C++26 reflection now (design accommodates it; no implementation).
- C# interop exports (`scripting/interop/`) — a separate boilerplate surface, out of scope.
- Replacing bespoke inspector widgets (texture pickers, script editor) — they opt out.
