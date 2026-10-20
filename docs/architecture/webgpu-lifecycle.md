# Task 9 lifecycle and render boundary

Task 9 keeps the established native Full and UiShell initialization/shutdown unchanged. Full initializes platform/GPU/scene/assets/cameras/rendering/UI, links dependencies, starts animation/compute/audio, and joins rendering before GPU-backed teardown; audio stops before its VFS/assets. UiShell initializes only window/GPU/UI and its clear pass, without gameplay, cameras, scene passes or audio. Neither path depends on the browser backend.

Browser initialization owns World -> GameplayAccess -> linked Rust module, then asynchronously requests adapter/device and creates WebGPU resources. A browser animation callback runs input latch -> bounded fixed updates -> ECS extraction into an owned RenderFramePacket -> GPU submission. The backend consumes only the packet; it never touches World. Browser input here is the minimal keyboard latch needed for the M3 Rust movement gate; complete platform input/audio/save/package support belongs to Task 10.

Lifecycle states are Initializing, Ready, Failed, DeviceLost, Stopping and Stopped. No adapter/device/queue promise blocks the main thread. Stopping cancels frame scheduling immediately, detaches/destroys gameplay while its World/access are valid, drains submitted work asynchronously, and releases GPU-backed resources before device/surface/adapter/instance. Late initialization callbacks release their returned handles without restarting a stopped runtime. Device loss is terminal and cancels new submissions; callbacks must still retire owned frame allocations. Resize reconfigures the canvas on a frame boundary; zero extents suspend rendering. Visibility resume resets the fixed-step clock.

The first portable rendering operation is an ordered 2D sprite pass over RenderFramePacket.render2D: no Vulkan barrier, device address, timeline semaphore or descriptor heap enters its contract. Explicit frame/instance buffers use fixed layouts. A draw binds one texture/sampler; changing texture or blend starts another contiguous batch without reordering transparency. Instance-capacity overflow splits batches; total frame-budget overflow and unsupported features fail explicitly. Alpha/additive/multiply/opaque have explicit pipeline blend states. Hardware homogeneous clipping uses clip coordinates, with UV flips/masked/nearest/pixel-snap handling. Texture registration accepts bounded RGBA8 uploads; full target asset cooking is Task 10.

Qualification requires portable unit tests for layout/order/bounds/lifecycle; actual Rust SDK/provider linked into Wasm; real World mutation rendered through the shared packet; hardware Chromium pixel/resize/input/device-loss/unsupported-limit/shutdown checks with validation diagnostics; and native build/unit/GPU plus C# and Rust desktop fixture preservation.

## Build, run and inspect

Run the existing native host build once, activate the pinned SDK through the check script, and provide the pinned Playwright module:

```powershell
./scripts/Run-WebRuntimeChecks.ps1 -BrowserChecks `
    -EmsdkPath .temp/toolchains/emsdk-main `
    -SlangCompiler C:/VulkanSDK/1.4.350.0/Bin/slangc.exe `
    -PlaywrightModule <absolute-path-to-playwright>
```

`-SkipHostBuild` reuses verified host tools. `-BrowserExecutable` selects an installed Chromium executable; otherwise the runner uses Chrome. All qualification browsers are headless and closed in `finally`. The report distinguishes the Node foundation result from the browser renderer/keyboard result. It never treats a Node run as browser graphics evidence. Audio, storage and complete input remain unavailable. No CI trigger changes are introduced.

Serve `build/webgpu-browser/web/` on localhost or HTTPS and open its `index.html` to inspect the procedural M3 scene. Click the canvas and hold Left/Right. Rust's `BrowserMover` uses the existing safe SDK, services negotiation, provider and gameplay ABI; the host validates the module and manages create/attach/update/detach/destroy/shutdown. The application extracts actual World transforms and SpriteRendererComponents into the engine's RenderFramePacket. GPU code consumes that owned snapshot only. The fixture uses registered RGBA8 textures; scene loading, authored texture/atlas cooking and complete reference-game parity remain Task 10.

The canvas follows its CSS size, clamped by the explicit 4096-pixel extent limit. Zero extents suspend submission. Keyboard input is scoped to canvas focus; blur and visibility changes release keys and reset the fixed-step clock. A bounded update delta prevents an unbounded catch-up loop. The `test` URL parameter fixes render dimensions for deterministic pixel checks. Other test parameters select controlled unsupported-adapter/limit, teardown and scene scenarios; no browser security switches are required.

Shader and package outputs are explicit link dependencies of the Emscripten packager. With the SDK environment active, `python tools/web-runtime/check_asset_relink.py` temporarily changes a shader expression, verifies the new WGSL bytes are embedded in AetherGame.data, then restores and rebuilds the original shader even on failure. Timestamp-only edits may legitimately leave Slang's output unchanged.

## Lifetime and failure completion

Callback state is shared only for the existing asynchronous lifetime: initialization retains it, Stop prevents new work, in-flight frame callbacks retire their allocations after queue completion/readback, and the final device-lost callback releases its retention. Destroying the public owner during initialization cannot free callback userdata prematurely. A stopped status is published after callbacks, frames and the device-loss notification drain. Terminal failure/loss remains in `terminalState` and structured diagnostics while automatic application teardown proceeds; callers do not need a second Stop request to clean up a failed renderer.

The shader uses 80-byte frame constants and 128-byte sprite records. Each draw binds one texture/sampler and a contiguous range of the shared instance buffer. Ordered batches split at texture/blend changes or 1,024 instances; a frame above 65,536 instances fails explicitly. The backend supports alpha, additive, multiply and opaque pipelines, UV flips, pixel snap, nearest sampling and masked fragments. The first profile does not expose Vulkan bindless textures, addresses, barriers or timeline semaphore operations.

## Qualification record

Windows hardware Chromium qualification covers all 4,096 pixels of the four-quadrant texture, actual keyboard-to-Rust-to-World movement and clipped pixels, explicit and responsive resize, unsupported adapter/limits, terminal device loss with automatic teardown, stop during initialization, in-flight readback retirement, destruction of an initializing backend owner, shutdown after texture upload before the first draw, transparency/sort order, and a 1,026-instance scene split into three draws. Tests reject software/fallback adapters and uncaught page or unexpected validation errors. Reports retain GPU identity, browser/Playwright versions and diagnostics under logs/task9/.

Regression checks include the normal native build and gauntlet, C# and Rust desktop reference fixtures, source/import/shared-memory audits, and the shared sprite/lifecycle test registered in EngineTests. clangd and graphify are regenerated after the structural changes. Linux native, Firefox, Safari, macOS and hosted embeds remain unqualified; this gate makes no cross-browser or complete-export claim.

Task 9 qualification used Emscripten 6.0.12, Emdawnwebgpu v20260423.175430, Rust nightly-2026-10-18, Slang 2026.8 and Playwright 1.62.1. The final hardware browser suite passed 11 cases on NVIDIA Blackwell with Chrome 155.0.8059.85; source/import audits recorded 44 translation units, 66 Wasm imports and no shared memory. C# and Rust desktop reference games passed, the native GPU abstraction guard remained clean, and both hidden validation smokes had zero findings. The new sprite/lifecycle contract also runs in EngineTests with 23 assertions.

The final normal native suite passed all 1,455 cases after the new contract was registered, with its 23 assertions also checked independently. The integrated report is `logs/task9/integrated/report.json`; the final unit report is `logs/task9/final-unit-gauntlet.json` and native GPU validation is recorded in `logs/task9/final-native-gauntlet.json`.

## Task 10 continuation

The historical M3 qualification above is superseded for platform readiness by [browser assets and platform services](browser-assets-and-platform.md). Task 10 adds real packaged reference gameplay, audio, saves and shared input/UI integration. The release-excluded bridge is documented in [browser automation](browser-automation.md), and generic Rust publishing in [Web exports](../web-export.md).
