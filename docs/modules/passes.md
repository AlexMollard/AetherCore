# `passes/` - Render passes

Individual `RenderPass` nodes that are registered in the `RenderGraph`. Passes are owned by `RenderingSubsystem` and instantiated once at subsystem init.

## Files

| File | Role |
|---|---|
| `CullPass.hpp` / `CullPass.cpp` | GPU frustum + occlusion culling. |
| `ForwardPass.hpp` / `ForwardPass.cpp` | Main forward PBR shading. |
| `SkyboxPass.hpp` / `SkyboxPass.cpp` | Sky/horizon/zenith/void gradient. |
| `PostProcessStack.hpp` / `PostProcessStack.cpp` | Tonemap, bloom, etc. |

## How passes fit together

`RenderingSubsystem::Init` (`src/engine/rendering/RenderingSubsystem.cpp`) builds all passes and registers them on the render graph. The graph compiler topologically sorts them and resolves resource lifetimes. Typical order:

1. **CullPass** - reads the visibility stream from `RenderQueue`, writes a per-mesh visibility bitmask and indirect-draw arguments.
2. **Shadow passes** - sun and local light shadow map writes (driven by `ShadowService` and `LocalShadowService`).
3. **SkyboxPass** - writes the sky gradient at the far plane.
4. **ForwardPass** - reads the cull output and draws visible meshes with PBR shading, applying shadow maps and skinning matrices.
5. **PostProcessStack** - bloom, tonemap, color grading, final blit to swapchain.

The exact order can be inspected at runtime via `AetherCore::GetRenderPassNames()`.

## `CullPass`

Performs GPU-side visibility culling:

- Reads the `RenderQueue`'s draw command buffer.
- Tests each draw's bounding sphere against the frustum.
- For larger draws, optionally runs an occlusion test (HiZ).
- Outputs an indirect-draw argument buffer consumed by `ForwardPass`.

`CullPass` is registered in `RenderingSubsystem::RegisterPasses` and lives at the head of the frame graph.

## `ForwardPass`

The main PBR pass. Consumes:

- The cull output from `CullPass`.
- The sun and local light shadow maps.
- Per-frame `FrameConstants` (view, proj, sun, ambient, sky colors, point/spot light lists).
- Skinning matrices from the animation systems (via BDA in `RenderQueue`).
- Material data from `MaterialBuffer` (bindless).

Outputs to the HDR color attachment (canonical format: `GpuFormat::R16G16B16A16Sfloat`).

## `SkyboxPass`

Renders the sky gradient using the three colors set on `Renderer`:

- `skyHorizonColor`
- `skyZenithColor`
- `skyVoidColor` (used for a smooth void/dome below the horizon)

The pass writes to depth = far plane and is drawn before the forward pass.

## `PostProcessStack`

A chain of post-process passes - tonemap, bloom, optional film grain, color grading - that ends with a final blit to the swapchain image. The exact set is configurable per-application; the default stack is built in `PostProcessStack` constructor.

## `PhysicsDebugRenderer`

Lives under `src/engine/physics/PhysicsDebugRenderer.hpp`. It produces `DebugVertex` arrays that are routed through `RenderFramePacket::debugVertices` and rendered by a dedicated `$PhysicsDebug` pass added to the graph by `RenderingSubsystem`.

## Adding a new pass

1. Create `src/engine/passes/MyPass.hpp/.cpp` with a class that owns its pipeline and descriptor sets.
2. Add a member to `RenderingSubsystem` (`src/engine/rendering/RenderingSubsystem.hpp:130`).
3. Construct it in `RenderingSubsystem::Init`.
4. Register it on the render graph with the correct dependencies (must run after inputs are written, before consumers read them).
5. Add a getter to `RenderingSubsystem` for runtime introspection.
6. Document it here.
