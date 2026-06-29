# Modern Vulkan Render Graph Practices

Last reviewed: 2026-06-30
Target Vulkan version: 1.4.350.0

This note captures the Vulkan-facing rules AetherCore should enforce while upgrading the render graph. It is not a tutorial; it is the contract future render passes should fit into so adding GTAO, SSR, TAA, decals, transparency, and async compute work stays predictable.

Primary source: Khronos Vulkan docs, especially the Synchronization Examples wiki, Vulkan API reference, and Vulkan 1.4 proposal. Secondary practice source: Sascha Willems' "How to Vulkan in 2026" guide for modern Vulkan defaults.

## Baseline

AetherCore targets Vulkan 1.4.350.0 for the main renderer path. The architecture can support compatibility fallbacks later, but new render graph and Vulkan abstraction work should assume the Vulkan 1.4.350.0 path first.

- Vulkan 1.4.350.0 device path as the target.
- `synchronization2` / `vkCmdPipelineBarrier2` for barriers and queue dependencies.
- `dynamicRendering` for graphics pass recording.
- `dynamicRenderingLocalRead` where it simplifies local attachment read patterns.
- Descriptor indexing for bindless sampled images and variable descriptor arrays.
- Buffer device address for GPU contracts that already use BDA-style payloads.
- Timeline semaphores and `vkQueueSubmit2`-style submission for cross-queue render graph synchronization.
- `maintenance5` and `maintenance6` as part of the modern device baseline.
- `pipelineRobustness` where robustness policy needs to be explicit.
- `hostImageCopy` as an optional fast path for upload/readback workflows, not as a render graph dependency substitute.
- `pushDescriptor` only for narrow transient binding cases where it simplifies code without undermining bindless conventions.

Vulkan 1.2 and 1.3 features are listed in this note only when they form the foundation of the Vulkan 1.4 path.

## Render Graph Contract

Every pass must declare all GPU-visible work in data, not ambient side effects:

- Color, depth, storage, sampled, transfer, and present image usages.
- Buffer reads and writes, including indirect argument buffers.
- Queue class: graphics, compute, async compute, transfer.
- Whether the pass produces or consumes a prepared draw list.
- Whether the pass has non-resource side effects.
- Required viewport extent and sample count.
- Bindless resources read by shaders.

If a pass needs ordering but has no resource edge, it must declare a logical dependency. Side-effect-only ordering should be rare and validated loudly.

## Frame Ownership

Frame slot ownership is explicit:

- `RenderFramePacket.drawSlot` is the canonical slot for render execution.
- Frame constants, lighting buffers, prepared draw lists, render graph storage, bindless frame state, and async compute submission must use the same slot for a packet.
- The game thread may not reuse a frame slot until the render thread has completed the frame that last owned it.
- Validation should assert slot agreement at subsystem boundaries, not after visual corruption appears.

Recommended next abstraction:

```cpp
struct FrameResourceContext
{
	std::uint64_t frameIndex;
	std::uint32_t frameSlot;
	std::uint32_t swapchainImageIndex;
	gpu::DeviceAddress frameConstantsAddr;
	FrameTarget target;
};
```

Passes should read `ctx.frame.frameSlot`, not recompute modulo arithmetic locally.

## Prepared Draw Lists

Draw preparation should become a typed graph resource:

```cpp
PreparedDrawListHandle mainDraws = graph.ProduceDrawList("$CullDraws", mainQueue);
graph.AddPass("$ScenePreDepth").ConsumeDrawList(mainDraws);
graph.AddPass("$EngineForward").ConsumeDrawList(mainDraws);
```

This avoids hidden dependencies between cull and draw passes. A pass that consumes a prepared draw list should be ordered after its producer by the graph compiler, not by declaration luck.

## Synchronization2 Rules

Use `vkCmdPipelineBarrier2` / `VkDependencyInfo` style barriers for graph transitions. The graph should derive stage and access masks from resource usage, then validate them.

Important canonical transitions:

- Depth attachment write to fragment shader sample: source stages `EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS`, source access `DEPTH_STENCIL_ATTACHMENT_WRITE`, destination stage `FRAGMENT_SHADER`, destination access `SHADER_READ`, layout from attachment optimal to read-only optimal.
- Depth attachment write to compute shader sample: same source stages/access, destination stage `COMPUTE_SHADER`, destination access `SHADER_READ`.
- Color attachment write to shader sample: source stage `COLOR_ATTACHMENT_OUTPUT`, source access `COLOR_ATTACHMENT_WRITE`, destination shader stage matching the consuming pass, destination access `SHADER_READ`.
- Shader write to indirect draw: source shader stage and `SHADER_WRITE`, destination `DRAW_INDIRECT`, destination access `INDIRECT_COMMAND_READ`.
- Transfer write to shader read/write: source `TRANSFER`, source access `TRANSFER_WRITE`, destination shader stages, destination shader access.

Full `ALL_COMMANDS` / memory read-write barriers are allowed only as diagnostic escape hatches and should be tagged as such.

## Dynamic Rendering Rules

Render graph passes should own attachment metadata:

- Attachment format.
- Load/store op.
- Initial, render, and final layout.
- Clear value.
- Extent and sample count.

Graphics command recording should not depend on legacy render pass objects for pass identity. Pipelines may still need compatible color/depth formats, but graph scheduling and barriers should come from declared attachment usage.

## Bindless and Descriptor Indexing

Bindless descriptors are long-lived GPU references and need explicit lifetime tracking:

- A bindless slot must remain valid until every frame that can sample it has retired.
- Replacing a sampled image requires deferred destruction or a device idle boundary.
- A graph resource that is sampled through bindless must still be declared as a pass read.
- Descriptor writes must be visible before command buffers that sample them are submitted.
- Shaders should receive validity flags or sentinel slots for optional resources.

This is especially important for scene depth, GTAO, post-process history, shadow atlases, and hot-reloaded textures.

## Timeline Semaphore Rules

Timeline semaphores should model queue dependencies, not frame ownership:

- Async compute work signals a graph-owned timeline value.
- Graphics waits on that exact value at the earliest stage that consumes the compute output.
- CPU frame-slot reuse still follows render-thread completion and per-frame fences.
- Queue ownership transfers are required only when ownership changes and contents must be preserved.

The graph should expose timeline values in diagnostics so queue waits are visible in capture logs.

## Resource Lifetime

Destroying GPU resources directly is only safe after `WaitIdle()` or when the resource is provably unused.

Preferred model:

- Immediate destroy for startup/shutdown after GPU idle.
- Deferred destroy queue keyed by completed frame or timeline value for runtime resources.
- Bindless slot retirement coupled to deferred destruction.
- Render graph transient aliasing only after the last declared use of the previous occupant.

Swapchain and scene viewport rebuilds should wait idle until the engine has a robust deferred-destruction path for external sampled images.

## Validation To Add

The render graph should reject or warn on:

- Pass consumes a prepared draw list before its producer.
- Pass reads a texture/buffer without declaring it.
- Pass samples an attachment without a compatible read-only layout transition.
- Pass writes a bindless-exposed image while any in-flight pass can sample the previous contents.
- Pass has side effects but no declared dependency.
- Frame slot mismatch between packet, frame constants, queue, graph storage, and bindless state.
- Async compute output consumed without a timeline wait.
- Resource destroyed while still referenced by a bindless slot or in-flight graph frame.

## New Pass Checklist

When adding a pass:

1. Declare all image and buffer reads/writes.
2. Declare queue class and whether async compute is allowed.
3. Declare extent, format, sample count, load/store behavior, and clear values.
4. Declare prepared draw-list dependencies if drawing scene geometry.
5. Allocate bindless slots only through graph/bindless lifetime APIs.
6. Use `FrameResourceContext` values instead of recomputing frame slots.
7. Make optional resources explicit with validity flags.
8. Verify barriers in RenderDoc/Nsight once, then encode them as graph validation.

## Upgrade Order

1. Add `FrameResourceContext`.
2. Add typed prepared draw-list handles.
3. Add pass contract flags for side effects and logical dependencies.
4. Add graph validation for frame-slot and resource usage.
5. Add deferred destruction tied to completed frame/timeline state.
6. Add a render graph debug export showing ordered passes, resources, barriers, queues, timeline waits, and frame slot.
