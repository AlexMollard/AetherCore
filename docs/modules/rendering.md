# `rendering/` - Render graph, queue, and frame coordination

The `rendering/` module owns everything that turns scene state into GPU commands. It is the largest module in the engine.

## Files

| File | Role |
|---|---|
| `RenderingSubsystem.hpp` / `.cpp` | The subsystem façade. Owns all passes and the render graph. |
| `RenderGraph.hpp` / `.cpp` | DAG of `RenderPass` nodes. Computes order, handles resource aliasing. |
| `RenderQueue.hpp` / `RenderQueue.cpp` | Draw command collection + sort. |
| `Renderer.hpp` / `Renderer.cpp` | Per-frame state aggregator (camera, lights, world). |
| `RenderFramePacket.hpp` | Per-frame snapshot passed engine thread → render thread. |
| `RenderThread.hpp` / `RenderThread.cpp` | Dedicated render thread + `coro::channel<RenderFramePacket>`. |
| `FrameConstants.hpp` | The `FrameConstants` struct uploaded to GPU. |
| `FrameConstantsBuffer.hpp` / `.cpp` | The triple-buffered uniform buffer that hosts `FrameConstants`. |
| `RenderTargetService.hpp` / `.cpp` | Allocates render targets for offscreen cameras. |
| `RenderPipelineCoordinator.hpp` / `.cpp` | Coordinates the per-pass pipeline state. |
| `GraphicsPipeline.hpp` / `.cpp` | Pipeline cache + factory. |
| `ShadowService.hpp` / `ShadowService.cpp` | Cascaded sun shadow maps. |
| `ShadowAtlasManager.hpp` / `.cpp` | Atlas for sun shadow cascades. |
| `LocalShadowService.hpp` / `LocalShadowService.cpp` | Point/spot light shadow maps. |
| `LightingManager.hpp` / `.cpp` | Sun, ambient, sky, and local light state. |
| `FrameComposer.hpp` / `FrameComposer.cpp` | Final composition of the frame. |
| `CommandRecorder.hpp` / `CommandRecorder.cpp` | Thin wrapper over `VkCommandBuffer` for passes. |
| `GpuContracts.hpp` | Shared GPU-facing layout contracts. |
| `GpuTimestampPool.hpp` / `GpuTimestampPool.cpp` | GPU timestamp queries. |
| `WorldRenderer.hpp` / `WorldRenderer.cpp` | World-level render orchestration. |
| `PassResourceCompiler.hpp` / `PassResourceCompiler.cpp` | Compiles per-pass resource requirements into the graph. |

## `RenderFramePacket` (the snapshot)

`src/engine/rendering/RenderFramePacket.hpp:16`. Value-typed - copied through a `coro::channel`:

```cpp
struct RenderFramePacket {
    glm::mat4  view{1.0f};
    glm::mat4  proj{1.0f};
    glm::vec4  cameraWorldPos{0.0f};
    bool       hasCameraData = false;

    glm::vec4  sunDirectionIntensity{0.0f, -1.0f, 0.0f, 1.0f};
    glm::vec4  ambientColor{0.2f, 0.2f, 0.2f, 1.0f};
    glm::vec4  sunColor{1.0f};
    glm::vec4  skyHorizonColor{1.0f};
    glm::vec4  skyZenithColor{0.5f, 0.7f, 1.0f, 1.0f};
    glm::vec4  skyVoidColor{0.0f};

    std::vector<Renderer::PointLight> pointLights;
    std::vector<Renderer::SpotLight>  spotLights;
    std::vector<DebugVertex>          debugVertices;

    std::uint64_t materialBufferAddr = 0;
    std::uint64_t frameIndex         = 0;
    std::uint32_t drawSlot           = 0;
    float         elapsedTime        = 0.0f;
};
```

**Built on the engine thread** (in `AetherCore::PrepareFrame`), **consumed on the render thread** (in `AetherCore::ExecuteRenderFrame`). No data races with the next simulation tick.

## `RenderThread`

`src/engine/rendering/RenderThread.hpp:25`. Dedicated render thread that owns all Vulkan submission work.

```
Game thread   : Sim N -> SubmitFrame(N) -> Sim N+1 -> SubmitFrame(N+1) -> ...
Render thread :              Read N -> Exec N            Read N+1 -> Exec N+1 -> ...
```

- `Start(AetherCore&)` - spawns the worker.
- `SubmitFrame(RenderFramePacket)` - non-blocking, microsecond latency.
- `WaitIdle()` - block until no pending work. Call before destroying Vulkan resources.
- `SetReloadInProgress(bool)` / `IsReloadInProgress()` - pause/resume for hot-reload.

The channel is bounded to capacity 2 (double-buffered). Deep copy in the packet eliminates races.

## `RenderGraph`

`src/engine/rendering/RenderGraph.hpp`. Holds a DAG of `RenderPass` nodes. Each pass declares its inputs, outputs, and (implicitly via dependencies) execution order. The graph compiler:

1. Topologically sorts the passes.
2. Resolves transient resource lifetimes (which resources can alias the same physical memory).
3. Inserts barriers between passes.

This is what makes the renderer declarative - subsystems add passes, and the graph handles ordering.

Modern pass work should follow the Vulkan 1.4 render graph plan in [`docs/plans/modern-vulkan-render-graph-practices.md`](../plans/modern-vulkan-render-graph-practices.md). In particular, non-resource readiness such as prepared `RenderQueue` state should use typed graph contracts like `PreparedDrawList`, while image and buffer declarations remain responsible for synchronization2 barrier generation.

Recommended direction for new passes:

1. Declare every image and buffer access.
2. Declare non-resource dependencies through typed frame products, not ordering luck.
3. Prefer pass templates for common fullscreen, depth-only, queue-prepare, draw-queue, compute-image, and temporal shapes.
4. Keep optional resources explicit through frame blackboard lookups and validity flags.
5. Make pass contracts visible in the render graph debug panel and compile export.

## `RenderQueue`

`src/engine/rendering/RenderQueue.hpp:80`. Collects `DrawCommand` structs on the engine thread (one per visible mesh), then sorts them into a GPU-friendly order for `ForwardPass`. Each `DrawCommand` references:

- `Mesh` + `meshGeneration`
- `GraphicsPipeline`
- `materialIndex` + `modelMatrix`
- `animDb`, `animDbGeneration`, `animClipIndex`, `animTime`, `skinIndex`, `skinJointCount` (skinned draws)
- `worldBoundingSphere` (for culling)
- `instanceCount`

The queue holds pointers to the `AnimationBlendSystem` / `AnimationRootMotionSystem` so the forward pass can read the GPU skinning matrices via BDA.

## `Renderer`

`src/engine/rendering/Renderer.hpp`. Per-frame state aggregator. Updated by `CameraSubsystem` and `LightingManager`, snapshotted into `RenderFramePacket`.

Exposes:

```cpp
class Renderer {
public:
    void SetSun(const glm::vec4& dirIntensity, const glm::vec4& color);
    void SetAmbient(const glm::vec4& color);
    void SetSky(const glm::vec4& horizon, const glm::vec4& zenith, const glm::vec4& skyVoid);
    void SetPointLights(std::span<const PointLight>);
    void SetSpotLights(std::span<const SpotLight>);

    struct PointLight { /* pos, color, radius, intensity */ };
    struct SpotLight  { /* pos, dir, color, cone, intensity */ };
};
```

`LightingManager` is a higher-level façade that accumulates per-frame light additions from game code and pushes them into `Renderer` at the right time.

## Shadow services

- `ShadowService` (`src/engine/rendering/ShadowService.hpp`) - sun cascaded shadow maps. Owns `ShadowAtlasManager`.
- `LocalShadowService` (`src/engine/rendering/LocalShadowService.hpp`) - point and spot light shadow maps.

`RenderQueue`'s cull output drives both.

## `FrameConstants` and `FrameConstantsBuffer`

`FrameConstants` (`src/engine/rendering/FrameConstants.hpp`) is the per-frame uniform data uploaded to a triple-buffered uniform buffer. `FrameConstantsBuffer` (`src/engine/rendering/FrameConstantsBuffer.hpp`) is the GPU buffer wrapper.

The buffer is rotated per `frameIndex` so the GPU can read frame N while the CPU writes frame N+1.

## `RenderTargetService`

Allocates render targets for offscreen cameras (`CameraRenderTarget`). The `AetherCore::CameraRenderTarget` handle is a `uint32_t id`; pass it through to `RenderTargetService` to acquire/release the underlying image.

## `CommandRecorder`

Thin wrapper over `VkCommandBuffer` with a friendlier API for passes. Passes receive one per frame and call `cmd->BindPipeline(...)`, `cmd->Draw(...)`, etc. The wrapper never crosses the engine/render thread boundary.

## Threading

| Operation | Thread |
|---|---|
| `RenderQueue::Submit` | engine thread |
| `RenderFramePacket` build | engine thread |
| `RenderThread::SubmitFrame` | engine thread |
| `RenderFramePacket` consumption | render thread |
| `RenderGraph` execution | render thread |
| `GpuDevice::BeginSwapchainFrame` / `SubmitAndPresent` | render thread |

The engine thread **never** records GPU commands. The render thread **never** touches the ECS.

## See also

- [`modules/passes.md`](passes.md) - the individual render passes.
- [`modules/scene.md`](scene.md) - how the scene produces draw commands.
- [`modules/camera.md`](camera.md) - camera and lighting data flow.
