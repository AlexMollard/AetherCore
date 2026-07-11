# Render-frame extraction (the Frostbite model)

**Status:** adopted 2026-07-12. Migration in progress.

## The invariant

> **The render thread never reads mutable game state. The producer (game) thread
> extracts everything the renderer needs into an immutable `RenderFramePacket`
> once per frame; the render thread consumes only that packet.**

This is the "immediate / extract-and-consume" architecture used by Frostbite and
most console engines, as opposed to Unreal's "retained proxy + render command
queue" model. We chose it because:

- The engine was already ~80% there (`RenderFramePacket` + double-buffered draw
  slots), so this *finishes* a pattern instead of introducing a new framework.
- It makes the invariant **structural, not disciplinary**: once
  `ExecuteRenderFrame` has no `World*`, the render thread *cannot* touch the ECS,
  and the whole class of "render thread reads the world while the producer
  mutates it" races is gone by construction. No `RunExclusive`-for-reads, no
  per-system gating, no "is this toggle on?" reasoning.
- It scales along axes that don't require a rewrite: parallelize extraction
  across worker threads (one extractor per subsystem), and cache expensive
  extractors incrementally where a profiler says to. Unreal's retained model pays
  for delta-updates we don't need at this scale.

Structural ECS changes (spawn / destroy / add-remove-component), which are a
*different* problem, are handled by a deferred command buffer applied at a sync
point (see "Future: WorldCommandBuffer"), NOT by the render packet.

## Where the extract happens

`AetherCore::PrepareFrame` (producer thread, called from `RunFrameLoop`) is the
extract phase. It already builds, into per-slot buffers + the packet:

- mesh + skinned draw lists (`WorldRenderer::Flush` -> `RenderQueue`)
- render-target-service queues, directional + local shadow queues
- point/spot light lists (`packet.pointLights` / `packet.spotLights`)
- camera view/proj/near (`packet.view` / `packet.proj` / ...)

`ExecuteRenderFrame` / `EndFrame` / every `RenderGraph` pass consume the packet
and the per-slot buffers. `LightingManager::PrepareForRenderGraph` and both
shadow services' `BuildFrameShadowData` already read from the packet.

## Audit: render-thread world reads (2026-07-12)

| Site | Reads world on render thread? | Status |
|------|-------------------------------|--------|
| `WorldRenderer::Flush` (mesh/shadow draw extraction) | No - runs in `PrepareFrame` (producer) | OK |
| `LightingManager::PrepareForRenderGraph` | No - reads `packet.pointLights/spotLights` | OK |
| `ShadowService::BuildFrameShadowData` | No - packet only | OK |
| RenderGraph pass execute lambdas | No | OK |
| `UiRenderer::BuildFrame` -> `ResolveCanvases` | **Was yes** | Fixed `ef13e852` (moved to producer) |
| `LocalShadowService::BuildFrameShadowData(World&)` | No - param is `(void) world;`, dead | Remove dead param |
| `PhysicsDebugRenderer::DrawPhysicsDebugShapes` | **Yes** - `world->View<Collider,PhysicsState,RigidBody>().each` in the pass (render thread), gated `s_physicsDebugShapesEnabled` off by default | Extract instances to packet |

Net: the migration is surgical, not a rewrite.

## Migration phases

1. **Drop the dead `World&`** from `LocalShadowService::BuildFrameShadowData`
   and its call site.
2. **Extract physics-debug shapes on the producer**: build a
   `std::vector<PhysicsDebugInstance{model, tint, shapeType}>` into the packet;
   the pass replays it (picking the static per-shape vertex buffer by type) with
   no world access.
3. **Remove `World&` / `SceneSubsystem::GetWorld()` from `ExecuteRenderFrame`**
   (and the `debugRenderer.SetWorld` call). The render thread now holds no world
   pointer - the invariant is compiler-enforced. Add a one-line assert/comment
   at the top of `ExecuteRenderFrame` documenting it.

## Future: WorldCommandBuffer (structural changes)

Deferred structural mutations - the Unity DOTS `EntityCommandBuffer` idea - so
scripts/systems can `spawn`/`destroy`/`add`/`remove` during a frame without
racing readers. Commands are recorded during the frame and flushed at one sync
point on the producer. This is also the clean home for the "better script entity
spawning" API. Additive; not required for the invariant above, but it closes the
structural-change race class the same way extraction closes the read race class.
