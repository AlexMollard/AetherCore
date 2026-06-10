# `scene/` - ECS, world, scene

The `scene/` module hosts the entity/component system. It is the engine's source of truth for game state on the engine thread.

## Files

| File | Role |
|---|---|
| `World.hpp` / `World.cpp` | Wraps EnTT 3.16. |
| `Scene.hpp` / `Scene.cpp` | Higher-level façade for renderable entities. |
| `SceneSubsystem.hpp` / `SceneSubsystem.cpp` | Owns `World` and `Scene`, registers them on the service container. |
| `Components.hpp` | ECS component definitions (transform, mesh, material, light, etc.). |
| `Entity.hpp` | Typed entity handle. |
| `EcsHelpers.hpp` | Typed accessors and convenience functions. |
| `TagSlots.hpp` | Tag-based entity lookup. |
| `LoadedModel.hpp` | The data structure that holds a loaded glTF model. |
| `System.hpp` / `System.cpp` | The per-frame `Update(dt)` interface for game systems. |

## `World`

`src/engine/scene/World.hpp`. Thin wrapper over EnTT's `entt::registry`. Provides:

- Entity creation / destruction.
- Component attach / detach / query.
- Per-frame system dispatch.

**Thread safety:** All `World` operations are engine-thread-only. The render thread never touches it.

## `Scene`

`src/engine/scene/Scene.hpp`. A higher-level façade built on top of `World`. Provides:

- Named scenes (level loading).
- Entity hierarchy helpers.
- Tag-based lookup (`TagSlots.hpp`).
- Integration with `AssetManager` for entity ↔ model binding (`LoadedModel.hpp`).

## `SceneSubsystem`

`src/engine/scene/SceneSubsystem.hpp`. Owns the `World` and `Scene` and registers both on the service container. Has no `Shutdown` work - destruction order is handled by the `std::unique_ptr` in `AetherCore`.

## Components

`src/engine/scene/Components.hpp`. The component types used by the engine and game:

- `Transform` - position, rotation, scale, parent.
- `Mesh` / `MeshInstance` - handle to a `MeshArena` slot + generation counter.
- `Material` - handle to `MaterialBuffer`.
- `Light` (point / spot) - color, intensity, radius / cone.
- `Camera` - view/proj override (or use `CameraManager` for full cameras).
- `RigidBody`, `Shape` - physics.
- Animation tags (`AnimationTag`, `SkinJointCount`, etc.) - wired to `AnimationBlendSystem` / `AnimationIkSystem`.

## `System`

`src/engine/scene/System.hpp`. The per-frame update interface:

```cpp
class System {
public:
    virtual ~System() = default;
    virtual void Update(World& world, float dt) = 0;
};
```

Game systems inherit from this and are registered with `World` for automatic dispatch on `Tick`.

## How the scene feeds the renderer

The engine thread runs game systems, which mutate ECS state. At `PrepareFrame` time, the rendering subsystem iterates the world to build a `RenderQueue` of `DrawCommand` entries. These are snapshotted into the `RenderFramePacket` (or rather, into the queue that the render thread will consume).

The render thread **never** re-queries the ECS.

## See also

- [`modules/rendering.md`](rendering.md) - how `RenderQueue` consumes the scene.
- [`modules/physics.md`](physics.md) - physics components.
- [`modules/animation.md`](animation.md) - animation components.
- [`modules/camera.md`](camera.md) - camera and lighting.
