# Web exports

The publisher builds the selected Rust or C# project for `webgpu-2d-v1`, packages its reachable content and verifies the extracted ZIP in hardware Chrome before replacing the previous export. Desktop remains the default target and existing projects still default to C#. C# browser exports use the trimmed interpreter described in [C# browser support](csharp-web.md).

## Publish from an agent or the editor

```powershell
$env:AETHER_WEB_SLANGC = 'C:/VulkanSDK/1.4.350.0/Bin/slangc.exe'
$env:AETHER_PLAYWRIGHT_MODULE = '<absolute-path-to-pinned-playwright>'
./scripts/Run-GameplayPipeline.ps1 -Project projects/AgentPipelineRust `
    -Target web -Operation publish -Scene ReferenceStart `
    -ReportPath logs/web-publish-report.json
```

Omit `-Scene` to use `[app].startupscene`. `-Async` returns a persistent job ID; use `-Operation status -JobId <id>` or `-Operation cancel -JobId <id>`. Web publishing defaults to a ten-minute overall deadline; `-TimeoutMs` can shorten it. `gameplay_job_start` accepts `target: "web", operation: "publish"`; its status/cancel tools use the same jobs and terminal AgentReport v1. Restart the MCP client after updating server code so the target enum refreshes. Web build/run/test operations remain unavailable through this command; the browser qualification runner has its own interface.

In the editor Build panel, select **Web** and publish. The existing `project.publish` control method also accepts `{ "target": "web" }`; `project.publish_status` reports progress and the verification evidence. Omitting target preserves native publishing. Both entry points call the same publisher. Web publishing currently requires the matching engine source checkout and installed host toolchain; the resulting game has no checkout dependency.

## Prerequisites

Build native `AssetPacker`, and install the exact versions in [toolchain.json](../tools/experiments/rust-webgpu/toolchain.json): Emscripten/emdawnwebgpu, Slang and Playwright. Python with `tomllib`, CMake, Node and installed hardware Chrome must be available. Rust games additionally require Cargo and the pinned Wasm nightly; C# games require the pinned .NET browser toolchain in [C# browser support](csharp-web.md). Publishing does not install tools or browsers. `EMSDK` selects the SDK (local fallback `.temp/toolchains/emsdk-main`); `AETHER_WEB_ASSETPACKER` and `AETHER_WEB_SLANGC` override host tools. `AETHER_PYTHON` selects the Python executable. `AETHER_PLAYWRIGHT_MODULE` selects the pinned Playwright module directory. Software GPU execution does not pass required verification. See [toolchain qualification](architecture/rust-webgpu-toolchain.md).

Each project gets a separate `build/web-publish-<identity>` cache. A project-scoped lock serializes publishers sharing that cache. Rust links as a static library with Web-only abort panic policy; desktop Cargo profiles and C# compilation/reload remain unchanged.

## Content declarations and preflight

```toml
[publish.web]
profile = "webgpu-2d-v1"
id = "my-studio.my-game"
instructions = "WASD to move. Space to dash. R to retry."
scenes = ["Start", "Level2", "Results"]
additional_assets = ["project://assets/audio/theme.wav"]
capabilities = ["sprites", "audio", "saves", "scenes"]
```

The optional ID namespaces browser storage; otherwise the product name is used, so give distinct games distinct IDs. Static Rust/C# scene, prefab and project asset references supplement the declarations. Dynamic scene names require `scenes`; dynamic audio/prefab paths require `additional_assets`. The browser refuses scene loads outside the packaged scene list. Include every dynamically reachable asset explicitly. Preflight scans authored source and reachable files conservatively; it does not prove every possible gameplay branch executes correctly.

The profile supports 2D sprites, animation, tilemaps, Box2D, supported UI, packaged audio, input, scene/prefab loading and asynchronous browser saves. Scenes must declare `kind = "2d"`; authored cameras must be orthographic. Headerless 2D prefab fragments remain supported. UI currently uses packaged Roboto and Latin-1 glyphs. Custom shaders/materials, 3D content, networking and unsupported component tables are rejected. Native BC texture caches are not reused; the Web asset cooker produces target-qualified RGBA8 textures.

Specific failures include `WEB_SCENE_MISSING`, `WEB_DYNAMIC_SCENE_UNDECLARED`, `WEB_DYNAMIC_ASSET_UNDECLARED`, `WEB_ASSET_PATH`, `WEB_ASSET_MISSING`, `WEB_ASSET_FORMAT`, `WEB_CAPABILITY_UNSUPPORTED`, `WEB_NETWORK_UNSUPPORTED`, `WEB_SHADER_UNSUPPORTED`, `WEB_FEATURE_UNSUPPORTED`, `WEB_FONT_UNSUPPORTED` and `WEB_TOOLCHAIN_MISSING`. Build/verification failures include paths to their logs. Never interpret a failed or missing required check as a usable release.

## Package and evidence

Output is `Builds/Web/<product>/<product>.zip`, with `index.html` at the ZIP root. It includes the relative loader, Wasm/data, target PAKs, project configuration and manifest v1. The manifest records file byte counts/SHA-256, selected project/startup/scenes, engine Git checkpoint and pinned toolchains. The checkpoint identifies HEAD at build time; it is not an attestation that the source checkout was clean.

Publishing stages content privately, extracts the ZIP into a separate directory and serves it at a nested URL with Wasm MIME type. Required checks cover file integrity, absence of test bridge/fault exports, real hardware WebGPU startup and bounded frames, a trusted Start gesture, local iframe execution, and actionable missing-WebGPU failure. Generic verification does not impose the reference game's score, save count or scene flow. The separate [41-case reference qualification](architecture/browser-automation.md) covers those behaviors and fault cases.

The terminal report links `web-verification.json`, logs, manifest and ZIP; browser evidence includes environment, state, screenshot and cleanup. Failed builds or browser verification leave the previous valid export intact. The final directory is swapped only after successful verification. Interrupted jobs can leave hidden staging directories for inspection; they do not expose a partial package as the final export.

Serve the extracted directory over HTTPS or localhost; opening `index.html` through `file://` is not supported. Use a WebGPU-capable hardware browser and appropriate iframe permissions (`autoplay; fullscreen; gamepad`, plus host-specific WebGPU policy when required). Start enables audio through a real gesture. Local nested-URL/iframe verification does not establish support on itch.io or another external host. Use [autonomous distribution qualification](distribution-qualification.md) for Task 13 measurements and the browser/host matrix. External hosting remains pending until an authorized uploaded draft is exercised.

## Playable sample and local preview

[Ember Run](../projects/EmberRun/README.md) is a Rust collect-and-dodge sample with
movement, dash, three lives, 30-second rounds, retry and a saved best score. It runs
through the same shared gameplay host as other games. The empty development preview
URL selects Ember Run; explicit reference-fixture URLs retain their deterministic
regression behavior. Publish the sample using the normal project command above.

The player uses the real editor icon and the default Night Amber colors. Play focuses
the canvas immediately, including inside an iframe. Clicking outside releases keyboard
input. Optional `publish.web.instructions` supplies the game's controls below the canvas.
The technical state stays in a collapsed details panel.

For sample-specific browser qualification after publishing:

```powershell
node tools/browser-tests/verify-export.mjs `
    '--root=projects/EmberRun/Builds/Web/Ember Run' `
    --report=logs/ember-browser.json --scenario=ember-run-v1
```

This checks immediate keyboard movement/scoring, audible collection, retry, a completed
round and best-score persistence after reload, alongside normal export/iframe checks.
Set `AETHER_PLAYWRIGHT_MODULE` to the pinned dependency as for publishing.
