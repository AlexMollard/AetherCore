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

Draw preparation is a typed graph contract. A cull or queue-prepare pass produces a `PreparedDrawList`; every pass that flushes that queue consumes the same handle:

```cpp
PreparedDrawList mainDraws = graph.CreatePreparedDrawList("MainSceneDraws");
cullPass.RegisterPass(graph, mainQueue, {}, mainDraws);
graph.AddPass("$ScenePreDepth").ConsumesDrawList(mainDraws);
graph.AddPass("$EngineForward").ConsumesDrawList(mainDraws);
```

This avoids hidden dependencies between cull and draw passes. A pass that consumes a prepared draw list is ordered after its producer by the graph compiler, not by declaration luck or string prefix coupling.

Rules:

- Prepared draw lists are not GPU resources; they model CPU/GPU queue side effects that normal image and buffer barriers cannot see.
- Every prepared draw list should have exactly one producer and at least one consumer.
- Dynamic systems, such as render-to-texture targets, must retire prepared draw-list handles when their passes are removed.
- Debug tooling should expose produced and consumed draw lists next to pass source location, side effects, resources, and barriers.

## Typed Frame Products

Prepared draw lists are the first typed non-resource graph contract. The same pattern should be extended to other frame products that are expensive to debug when they are implicit:

| Handle | Produced by | Consumed by | Purpose |
|---|---|---|---|
| `PreparedDrawList` | Cull / queue prepare passes | Predepth, forward, shadow, RTT draw passes | Orders side-effectful `RenderQueue` preparation before flushing. |
| `PreparedLightGrid` | Tiled/clustered lighting build | Forward, transparent, decals, debug views | Ensures lighting GPU buffers and per-view binning are ready. |
| `PreparedShadowData` | Shadow data composition | Forward, debug shadow views, post effects | Separates shadow metadata readiness from image layout transitions. |
| `SceneDepthProduct` | Predepth or forward fallback | GTAO, HZB, SSR, TAA, depth debug | Names the frame's canonical depth source and bindless slot. |
| `DepthPyramidProduct` | HZB build | Occlusion culling, SSR, volumetrics | Makes downsample chain readiness explicit. |
| `TemporalHistoryProduct` | TAA/history resolve | TAA, motion blur, temporal denoisers | Couples current/history images with frame-index validity. |
| `SceneViewProduct` | Frame composition / camera setup | View-dependent compute and draw passes | Carries view constants, extent, frame slot, and camera identity. |

Typed handles should not replace image and buffer declarations. They describe higher-level readiness and ownership; resource access declarations still drive Vulkan barriers.

## Frame Blackboard

Add a typed frame blackboard so passes request named products instead of threading ad hoc handles through subsystem code:

```cpp
auto& blackboard = graph.GetBlackboard();
const SceneDepthProduct sceneDepth = blackboard.Require<SceneDepthProduct>();
PreparedLightGrid lightGrid = blackboard.Create<PreparedLightGrid>("MainLightGrid");
```

The blackboard should:

- Store typed frame products with stable debug names.
- Reject duplicate producers unless the type explicitly allows replacement.
- Track source pass, consumer passes, frame slot, extent, format, bindless slot, and optional history validity.
- Make optional products explicit with `TryGet<T>()` instead of sentinel integers scattered through passes.
- Feed the render graph debug panel and JSON/DOT export.

## Pass Contract Validation

Render graph compile should act like a lint pass. It should warn or fail before frame execution when a pass contract is incomplete:

- Pass has no execute callback.
- Pass declares side effects without a reason.
- Side-effectful pass can be culled accidentally.
- Pass consumes a typed handle with no producer.
- Pass produces a typed handle with no consumer.
- Pass reads a graph image, buffer, or bindless sampled image without declaring the read.
- Pass writes a bindless-exposed image while older frames may still sample the previous contents.
- Pass uses async compute but writes/reads resources that require graphics-only stages.
- Pass consumes async compute output without a timeline wait.
- Pass extent, sample count, attachment format, or load/store policy is missing where graphics recording needs it.
- Pass debug-disabled callback is missing for queue-preparation passes that must discard pending work.

Validation should run in dev builds by default. Retail can strip most warnings, but the graph should still keep hard safety checks that prevent undefined behavior.

## Debug Export and Labels

Every compile should be exportable as a small artifact for debugging:

- Ordered pass list, source file/line, queue class, culled/disabled state.
- Resource lifetimes, aliases, final layouts, barriers, waits, and signal points.
- Typed handle producers/consumers.
- Transient heap allocations and alias groups.
- Async compute timeline semaphore values.
- Bindless slots sampled by each pass.

The Vulkan backend should also label work aggressively:

- `vkSetDebugUtilsObjectNameEXT` for graph images, buffers, pipelines, semaphores, events, and command buffers.
- Command buffer labels around every pass.
- Queue labels for graphics and async compute submissions.
- Debug names should use the stable render graph pass/product names.

This makes RenderDoc, Nsight, Aftermath, and validation messages point at the engine concept that caused the issue, not just a raw Vulkan handle.

## Pass Templates

Common pass shapes should be built through helpers that declare the boring parts automatically:

| Template | Defaults |
|---|---|
| `AddFullscreenPass` | Fullscreen triangle, frame extent, color/depth reads, bindless heap binding. |
| `AddDepthOnlyPass` | Depth attachment write, no color, depth compare/write policy, prepared draw-list consumption. |
| `AddQueuePreparePass` | Graphics-queue compute, side-effect reason, prepared draw-list production, debug-disabled discard callback. |
| `AddDrawQueuePass` | Prepared draw-list consumption, color/depth attachments, bindless heap binding. |
| `AddComputeImagePass` | Storage image/buffer declarations, dispatch extent, async policy, sync2 usage mapping. |
| `AddTemporalPass` | Current/history products, validity flags, history output ownership. |

New passes should be unusual only when their work is unusual. Most future render features should be one of these templates plus shader-specific resource declarations.

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

- Pass consumes a prepared draw list or typed frame product before its producer.
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
4. Declare typed frame-product dependencies, including prepared draw lists.
5. Prefer a pass template over raw `AddPass`/`AddComputePass` when a template fits.
6. Allocate bindless slots only through graph/bindless lifetime APIs.
7. Use `FrameResourceContext` values instead of recomputing frame slots.
8. Make optional resources explicit with validity flags.
9. Verify barriers in RenderDoc/Nsight once, then encode them as graph validation.

## Upgrade Order

Completed foundation:

1. Add `FrameResourceContext`.
2. Add typed prepared draw-list handles.
3. Add pass contract flags for side effects and logical dependencies.
4. Add render graph debug-panel visibility for pass source, side effects, logical dependencies, frame context, and prepared draw-list contracts.

Recommended next plan:

1. Add a typed `FrameBlackboard` with `Create<T>()`, `Require<T>()`, and `TryGet<T>()`.
2. Move canonical frame products into the blackboard: scene depth, GTAO output, HDR color, shadow maps, local shadow atlas, tiled light buffers, and main scene view.
3. Add contract validation for missing execute callbacks, missing producers/consumers, side-effect culling risk, undeclared bindless reads, and missing debug-disabled queue cleanup.
4. Add graph compile export as JSON first, then DOT/Graphviz once the schema settles.
5. Add automatic Vulkan debug labels for passes, products, graph resources, and queue submissions.
6. Add pass templates for queue preparation, draw-queue flushing, fullscreen post, depth-only, compute-image, and temporal passes.
7. Add compile-only render graph tests that assert pass order, typed handle validation, pass removal behavior, and representative sync2 barrier mappings.
8. Add deferred destruction tied to completed frame or timeline state, then remove remaining runtime `WaitIdle()` rebuild paths where safe.
