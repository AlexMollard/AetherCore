# `physics/` - Jolt physics integration

The physics module is a thin wrapper around Jolt Physics 5.5.0. It exposes ECS components, a simulation system, and a debug renderer.

## Files

| File | Role |
|---|---|
| `PhysicsSystem.hpp` / `PhysicsSystem.cpp` | The simulation step. Owns the Jolt world. |
| `PhysicsComponents.hpp` | ECS components: `RigidBody`, `Shape`, etc. |
| `PhysicsDebugRenderer.hpp` / `PhysicsDebugRenderer.cpp` | Produces `DebugVertex` arrays for the `$PhysicsDebug` pass. |

## Components

`src/engine/physics/PhysicsComponents.hpp`. ECS-side definitions:

- `RigidBody` - body type (static / kinematic / dynamic), mass, friction, restitution.
- `Shape` - collision shape (box, sphere, capsule, mesh).
- `PhysicsBodyRef` - handle into the Jolt body store.

`RigidBody` + `Shape` are added to entities by the asset loader or by gameplay code. `PhysicsSystem` synchronizes ECS state into Jolt at the start of the frame and reads back updated transforms at the end.

## `PhysicsSystem`

`src/engine/physics/PhysicsSystem.hpp`. Owns the Jolt `PhysicsSystem` and `TempAllocator`. Per-frame flow:

1. **Sync** - copy ECS `RigidBody` / `Shape` deltas into Jolt.
2. **Step** - `Update(dt, collisionSteps)`.
3. **Readback** - write Jolt transforms back into the `Transform` component of each entity.

`PhysicsSystem` is registered on the service container and called from a game system in `Tick` (or from a dedicated `PhysicsLayer` in the app).

## `PhysicsDebugRenderer`

`src/engine/physics/PhysicsDebugRenderer.hpp`. Produces `DebugVertex` line/point data (collider wireframes, contact points) for a single frame. The data is routed through `RenderFramePacket::debugVertices` (engine thread) and rendered by the dedicated `$PhysicsDebug` render pass on the render thread.

## See also

- [`modules/scene.md`](scene.md) - ECS integration.
- [`modules/rendering.md`](rendering.md) - `DebugVertex` and `$PhysicsDebug` pass.
- [`ARCHITECTURE.md §10`](../ARCHITECTURE.md) - physics data flow.
