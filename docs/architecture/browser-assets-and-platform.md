# Browser assets and platform services

Task 10 extends the [WebGPU lifecycle](webgpu-lifecycle.md) with the `browser-webgpu-2d-v1` reference-game profile. The browser runs the existing Rust reference script through the shared GameplayHost, World, scene parser/reflection, Box2D, sprite animation, tile extraction and UI interaction systems. Native C# hosting and native Vulkan lifecycle remain in place.

## Try the reference game

Build and qualify with the pinned tools:

```powershell
./scripts/Run-WebRuntimeChecks.ps1 -BrowserChecks `
    -SlangCompiler C:/VulkanSDK/1.4.350.0/Bin/slangc.exe `
    -PlaywrightModule <absolute-path-to-playwright>
python -m http.server 18989 --bind 127.0.0.1 --directory build/webgpu-browser/web
```

Open `http://127.0.0.1:18989/?reference=1`. Click Start, focus the canvas and hold Space. The actual Rust reference game moves with Box2D, observes collision/trigger events, instantiates its pickup prefab, plays a WAV, commits two save records and changes to ReferenceResult. `?reference=1&resume=1` reads that result from the same origin in a fresh page. `?reference=1&scene=BrowserFeatures` demonstrates authored sprite animation, tile batches, UI image/text, toggle and slider interaction. The original procedural M3 scene remains available without `reference`.

This is a development fixture with a fixed Rust module and project identity. Rebuilding and refreshing updates the page. Automatic watching/reloading, hosted embed qualification and C# browser execution remain separate milestones. Generic Rust projects now use [target-aware Web publishing](../web-export.md). Task 11 adds the [release-excluded automation bridge and bounded verification command](browser-automation.md).

## Package and VFS contract

The host AssetPacker accepts `--target desktop|webgpu-2d-v1`; desktop remains the default. Web textures use a versioned AER8 header followed by RGBA8 unorm pixels. Desktop BC/DDS and web RGBA8 artifacts have separate incremental-cache identities. Unsupported image formats and invalid dimensions fail during cooking. UTF-8 names retain their bytes on Windows.

Project and engine packages download asynchronously. MemoryPak validates the existing PAK version, endian marker, index and content hashes, bounds, UTF-8 paths, zstd output sizes and the embedded target manifest before mounting immutable bytes. Missing, corrupt, future-version and desktop-target packages fail explicitly. Stop cancels outstanding fetches. The browser VFS exposes shared read/text/stream operations without IOThread; writes and glob enumeration are unsupported. Saves use IndexedDB rather than pretending the read-only asset mount is writable.

Limits are explicit: packages at most 256 MiB, individual stored asset output at most 64 MiB plus header allowance, cooked textures at most 4096 pixels per axis, 255 scene texture slots including reserved fixture/fallback slots, and a separate glyph slot. WebGPU preserves the prior 65,536-instance frame limit and 1,024-instance ordered batches; overflow fails rather than dropping draws. Atlas, animation and tileset parsers reject unsupported schemas and oversized metadata before allocating their declared arrays.

## Gameplay, input and UI

The browser executes a sequential frame: feed shared input, resolve UI layout/navigation/widgets, advance fixed 60 Hz gameplay/physics/animation, extract an owned RenderFramePacket, then submit WebGPU work. No render-side ECS access is introduced. Short keyboard presses remain latched until a simulation tick consumes them. Pointer clicks have a render-frame latch for widget interaction. Canvas/window blur and visibility transitions release input and reset timing; returning to a page cannot introduce a suspended-duration catch-up delta.

Keyboard codes map to the shared engine key space. Pointer coordinates account for CSS-to-canvas scaling. Standard Gamepad API buttons, triggers and sticks map to the existing deadzone/navigation input. Gamepad tests inject the navigator API; a physical controller is not qualified. Visibility tests control DOM visibility events and use real canvas focus transfer; they do not qualify operating-system tab suspension.

Basic UI uses native layout/widget behavior and deterministic canvas sort bias plus parent-before-child draw ordering. Images and flat button/toggle/slider/progress shapes are emitted into the sprite packet. Packaged Roboto loads through FontFace and Canvas2D into a static Latin-1 glyph atlas, then renders through WebGPU. Font names, text limits and unsupported glyphs fail explicitly. This initial text path supports Latin-1 and newlines, at most 4096 UTF-8 bytes per label, and sizes from 1 to 192 pixels. Platform font rasterization can differ from native output.

Advanced analytic text/shape effects, rounded widget rendering, text boxes, masks, 3D content, scene includes/linked prefab instances and scripted prefabs are outside this profile. Authored AudioSource components are explicitly rejected; the supported Rust audio service plays packaged WAV clips. Orthographic cameras and plain reference prefabs are supported. This is not complete native scene feature parity.

## Audio and durable saves

Start/resume invokes AudioContext.resume directly from the trusted button event, before awaiting decoding. All packaged WAV files are discovered and decoded; unknown clips fail. There are at most 32 simultaneous voices, and each clip is bounded to 120 seconds / 64 MiB decoded. Tests schedule two distinct asset paths, observe nonzero analyser samples, suspend the AudioContext and resume from another trusted gesture. They establish signal scheduling, not acoustic output from a physical speaker. Stop aborts service work, stops voices and closes the audio context; late decode completion cannot repopulate stopped service state.

IndexedDB opens asynchronously per project and origin. Writes stage UTF-8 byte-length-aware values, including embedded NULs. Flush runs one strict-durability read/write transaction; only transaction completion increments committedWrites. Transaction error/abort, denied storage and quota exhaustion produce structured failures. Gameplay pauses while save commits are pending, so a scene transition cannot read an uncommitted snapshot as a successful durable save. A fresh page verifies the committed score and `café\0agent` payload. Browser storage availability and quota still belong to the browser/origin.

## Qualification

Task 10 passes all 31 required hardware browser cases. Qualification uses Windows, Chrome 155.0.8059.85, NVIDIA Blackwell hardware WebGPU, Playwright 1.62.1, Emscripten 6.0.12, Emdawnwebgpu v20260423.175430, Rust nightly-2026-10-18 and Slang 2026.8. Required browser cases include asynchronous package failures/cancellation, target and schema rejection, fonts, real reference gameplay and save/reload, animation/tile/UI pixel checks, audio scheduling/resume, storage denial/quota, mapped gamepad behavior, focus/visibility and short-key/UI-order regressions. Fallback/software adapters do not satisfy the graphics gate.

The combined report is `logs/task10/integrated/report.json`; its Node foundation result is separate from browser evidence. It marks full audio/storage/input readiness only after every required browser case appears exactly once and passes. Import/source audits reject native hostfxr/Vulkan/socket leakage, worker activation and shared Wasm memory. The browser closure contains 113 translation units and requires no SharedArrayBuffer.

Native verification is recorded in `logs/task10/final-native-gauntlet.json`: 1,460 tests pass, Editor and GameRuntime build, and both hidden GPU smoke runs have zero warning/error findings (one benign diagnostic is retained). Rust and C# desktop reference scenarios pass independently. Thirteen Python checks cover host cooking and Wasm/source audits. clangd and graphify are regenerated after structural changes. Linux, macOS, Safari, Firefox, physical controllers and hosted iframes remain unqualified.
