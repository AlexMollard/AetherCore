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

This is a development fixture with a fixed Rust module and project identity. Rebuilding and refreshing updates the page. Automatic watching/reloading and hosted embed qualification remain separate milestones. Selected Rust and C# projects use [target-aware Web publishing](../web-export.md). Task 11 adds the [release-excluded automation bridge and bounded verification command](browser-automation.md).

## Package and VFS contract

The host AssetPacker accepts `--target desktop|webgpu-2d-v1|webgpu-3d-v1`; desktop remains the default. Web textures use a versioned AER8 header followed by RGBA8 unorm pixels. Desktop BC/DDS and web RGBA8 artifacts have separate incremental-cache identities. Unsupported image formats and invalid dimensions fail during cooking. UTF-8 names retain their bytes on Windows.

Project and engine packages download asynchronously. MemoryPak validates the PAK version, endian marker, index and content hashes, bounds, UTF-8 paths, zstd output sizes and embedded target manifest before mounting immutable bytes. Missing, corrupt, future-version and desktop-target packages fail explicitly. Stop cancels outstanding fetches. The browser VFS exposes shared read/text/stream operations and sorted glob enumeration over mounted package indexes and streamed assets. Allocation-reserved reads admit expanded output before allocating/decompressing it. Asset writes remain unsupported, as on native PAK mounts; saves use IndexedDB instead.

Package limits remain explicit: at most 256 MiB per package and 64 MiB plus header allowance per stored asset output. Cooked colour textures are bounded to 4096 pixels per axis; the scene texture table retains its 255 usable slots plus fallback. Font curves use separate RGBA32F textures, not a reserved Canvas glyph atlas. Sprite extraction retains its 65,536-instance frame limit and 1,024-instance ordered batches. Parsers and GPU uploads reject unsupported schemas and oversized resources rather than silently dropping draws.

## Gameplay, input and UI

The browser executes a sequential frame: feed shared input, resolve UI layout/navigation/widgets, advance fixed 60 Hz gameplay/physics/animation, extract an owned RenderFramePacket, then submit WebGPU work. No render-side ECS access is introduced. Short keyboard presses remain latched until a simulation tick consumes them. Pointer clicks have a render-frame latch for widget interaction. Canvas/window blur and visibility transitions release input and reset timing; returning to a page cannot introduce a suspended-duration catch-up delta.

Keyboard codes map to the shared engine key space. Pointer coordinates account for CSS-to-canvas scaling; pointer lock reports the browser's actual grant/release and supplies relative motion. Standard Gamepad API buttons, triggers and sticks map to the native input surface. Unsupported haptics fail explicitly; physical controllers remain unqualified. Portable clipboard methods are asynchronous and observe real browser permissions. Clipboard tests must use a verified non-backed headless-shell clipboard, never an unpreserved owner OS clipboard.

UI shares native `BuildDrawCommands`, layout/widget systems, rounded geometry, nested clipping, text boxes, image materials and effect ordering. The WebGPU pass executes after scene post-processing/debug rendering at output resolution. `FontRegistry` and native cooked font curves drive both glyph rendering and textbox metrics. UTF-8 selects available native glyphs, including cooked private-use icons, with native fallback for missing glyphs. Canvas2D/FontFace rasterization and the legacy Latin-1 atlas are removed.

The 3D profile also stages scene includes/linked prefab instances and scripted prefabs, bakes models and cooks material graphs and portable custom passes. UI shaders preserve the native fixed push layout and current-element image/curve resources through role-specific WGSL artifacts. Arbitrary global bindless accesses and raw SPIR-V are not portable shader inputs. See [Web exports](../web-export.md) and the [effect ledger](webgpu-effect-parity.md) for exercised coverage and platform limits.

## Audio and durable saves

Start/resume invokes `AudioContext.resume()` from the trusted button event. The browser reuses native miniaudio DSP in manual, no-device, no-thread mode, feeding a bounded AudioWorklet PCM sink with reused transfer buffers. Native volume/pitch/loop/bus, music fades, spatial parameters and authored Audio Source components share the same mixer. The enabled native decoders support WAV, MP3 and FLAC; Ogg/Vorbis is not enabled by this build. Source-closure checks require the browser-only device/thread exclusion flags, and the Wasm audit still rejects native imports/shared memory. `logs/parity-full/audio-runtime-proof.json` records post-mixer signal measurements, not physical speaker output or device-latency guarantees.

IndexedDB opens asynchronously per project and origin. Writes stage UTF-8 byte-length-aware values, including embedded NULs. Flush runs one strict-durability read/write transaction; only transaction completion increments committedWrites. Transaction error/abort, denied storage and quota exhaustion produce structured failures. Gameplay pauses while save commits are pending, so a scene transition cannot read an uncommitted snapshot as a successful durable save. A fresh page verifies the committed score and `café\0agent` payload. Browser storage availability and quota still belong to the browser/origin.

## Qualification

Task 10 passes all 31 required hardware browser cases. Qualification uses Windows, Chrome 155.0.8059.85, NVIDIA Blackwell hardware WebGPU, Playwright 1.62.1, Emscripten 6.0.12, Emdawnwebgpu v20260423.175430, Rust nightly-2026-10-18 and Slang 2026.8. Required browser cases include asynchronous package failures/cancellation, target and schema rejection, fonts, real reference gameplay and save/reload, animation/tile/UI pixel checks, audio scheduling/resume, storage denial/quota, mapped gamepad behavior, focus/visibility and short-key/UI-order regressions. Fallback/software adapters do not satisfy the graphics gate.

The combined report is `logs/task10/integrated/report.json`; its Node foundation result is separate from browser evidence. It marks full audio/storage/input readiness only after every required browser case appears exactly once and passes. Import/source audits reject native hostfxr/Vulkan/socket leakage, worker activation and shared Wasm memory. The browser closure contains 113 translation units and requires no SharedArrayBuffer.

Native verification is recorded in `logs/task10/final-native-gauntlet.json`: 1,460 tests pass, Editor and GameRuntime build, and both hidden GPU smoke runs have zero warning/error findings (one benign diagnostic is retained). Rust and C# desktop reference scenarios pass independently. Thirteen Python checks cover host cooking and Wasm/source audits. clangd and graphify are regenerated after structural changes. Linux, macOS, Safari, Firefox, physical controllers and hosted iframes remain unqualified.
