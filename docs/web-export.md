# Web exports

The publisher builds the selected Rust or C# project for the selected Web profile, packages its reachable content and verifies the extracted ZIP in hardware Chrome before replacing the previous export. `webgpu-2d-v1` remains the default; `[publish.web] profile = "webgpu-3d-v1"` selects the 3D profile. Desktop remains the default target and existing projects still default to C#. C# browser exports use the trimmed interpreter described in [C# browser support](csharp-web.md).

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

Project graphics keys follow native case-insensitive lookup. Publishing converts
them to the browser manifest’s case-sensitive field names, so authored exposure,
AO strength, colour grading and other supported post settings do not silently
fall back to renderer defaults.

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

`Assets.List` queries enumerate the content selected for the package; wildcard patterns do not implicitly make disk files reachable. Declare those files normally. `Assets.WriteText` destinations are output paths, not read dependencies; immutable asset mounts still reject the write at runtime.

The `webgpu-2d-v1` profile supports authored sprites, animation, tilemaps, Box2D, UI, packaged audio, input, scene/prefab loading and asynchronous browser saves. Scenes declare `kind = "2d"` and use orthographic cameras. Headerless 2D prefab fragments remain supported. UI shares native draw-command generation and font curves; UTF-8 selects the glyphs available in the native cooked font, with the same missing-glyph fallback. Native BC texture caches are not reused by WebGPU; the Web asset cooker produces target-qualified RGBA8 textures.

The `webgpu-3d-v1` profile also accepts perspective cameras, meshes, lighting, authored materials/effects, and the implemented 3D physics and animation components. Reachable models are baked with the native asset packer; material graphs and portable custom passes are cooked before packing. Custom passes use `include/CustomPass.slangh`; UI materials/effects use their native fixed-layout ABI and current-element resources. Original shader sources are required; arbitrary global bindless indexing and raw SPIR-V are not portable. Declare dynamically named UI shaders in `additional_assets`. The report identifies bounded or unqualified coverage rather than claiming every parameter range or combination is qualified. See the [effect parity ledger](architecture/webgpu-effect-parity.md).

Specific failures include `WEB_SCENE_MISSING`, `WEB_DYNAMIC_SCENE_UNDECLARED`, `WEB_DYNAMIC_ASSET_UNDECLARED`, `WEB_ASSET_PATH`, `WEB_ASSET_MISSING`, `WEB_ASSET_FORMAT`, `WEB_CAPABILITY_UNSUPPORTED`, `WEB_NETWORK_UNSUPPORTED`, `WEB_SHADER_UNSUPPORTED`, `WEB_FEATURE_UNSUPPORTED`, `WEB_FONT_UNSUPPORTED` and `WEB_TOOLCHAIN_MISSING`. Build/verification failures include paths to their logs. Never interpret a failed or missing required check as a usable release.

## Native networking through the gateway

Browser games use WebSocket transport to a self-hosted ENet gateway; native peers keep ENet. Declare the `network` capability and the public connection settings:

```toml
[publish.web.network]
gatewayUrl = "ws://127.0.0.1:8788/net"
targetAlias = "native"
authToken = "local-development"
```

Build and run the gateway locally, using the exact browser origin that will connect:

```powershell
cmake --build --preset default --target NetGateway
$env:Gateway__Token = 'local-development'
$env:Gateway__Origins = 'http://127.0.0.1:18989'
$env:Gateway__Targets__native__Host = '127.0.0.1'
$env:Gateway__Targets__native__Port = '27000'
dotnet build/default/tools/net-gateway/RelWithDebInfo/managed/NetGateway.dll
```

`Net.Connect("native", 27000)` uses the allowlisted target alias. A requested port that differs from the alias's configured port is rejected, not silently redirected. Room-code joins resolve the room's gateway-owned port instead. Replication, RPCs, prediction, ownership and disconnect reasons keep the native application protocol. Ping measures the full application route; it is zero until a real reply is available, not the partial UDP segment's RTT.

Native and browser peers must use matching application wire schemas/builds. The canonical field schema and Ping/Pong changes are not a promise of interoperability with older native peer binaries.

The default listener is loopback-only at `http://127.0.0.1:8788`; UDP hosting is limited to ports 27000–27100. `Gateway:AllowPublic=true` is an explicit opt-in for non-loopback listening/UDP hosting. Public sites need WSS/TLS and an exact origin allowlist. `authToken` is embedded in public `project.js`: it is an admission key, not a secret or player authentication. The gateway is bounded to 128 sessions and 64 peers per hosted session. Qualification covers localhost peers in both directions, not public deployment, LAN discovery, TURN or rendezvous services.


## Package and evidence

Output is `Builds/Web/<product>/<product>.zip`, with `index.html` at the ZIP root. It includes the relative loader, Wasm/data, target PAKs, project configuration and manifest v1. The manifest records file byte counts/SHA-256, selected project/startup/scenes, engine Git checkpoint and pinned toolchains. The checkpoint identifies HEAD at build time; it is not an attestation that the source checkout was clean.

Publishing stages content privately, extracts the ZIP into a separate directory and serves it at a nested URL with Wasm MIME type. Required checks cover file integrity, absence of test bridge/fault exports, real hardware WebGPU startup and bounded frames, a trusted Start gesture, local iframe execution, and actionable missing-WebGPU failure. Generic verification does not impose the reference game's score, save count or scene flow. The separate [41-case reference qualification](architecture/browser-automation.md) covers those behaviors and fault cases.

For package parity qualification, `tools/web-runtime/package-parity-checks.py --package <package-root> --publish-build <build-dir> --inputs <web-inputs> --sibling <build-dir> --report <report.json> --playwright <module-dir>` rebuilds a bridge-enabled sibling, verifies identical game and engine PAKs and publish-time gameplay input hashes, then compares native gameplay and lighting against that sibling. The separate bridge-free relocated capture proves readiness, not tick parity. The measured budgets in `tools/browser-tests/distribution-budgets-3d.json` apply to the qualification fixtures only.

The terminal report links `web-verification.json`, logs, manifest and ZIP; browser evidence includes environment, state, screenshot and cleanup. Failed builds or browser verification leave the previous valid export intact. The final directory is swapped only after successful verification. Interrupted jobs can leave hidden staging directories for inspection; they do not expose a partial package as the final export.

Serve the extracted directory over HTTPS or localhost; opening `index.html` through `file://` is not supported. Use a WebGPU-capable hardware browser and appropriate iframe permissions (`autoplay; fullscreen; gamepad`, plus host-specific WebGPU policy when required). Start enables audio through a real gesture. Local nested-URL/iframe verification does not establish support on itch.io or another external host. Use [autonomous distribution qualification](distribution-qualification.md) for Task 13 measurements and the browser/host matrix. External hosting remains pending until an authorized uploaded draft is exercised.

The bundled preview server sends `.js` and `.mjs` as `text/javascript`, and `.wasm` as `application/wasm`, including nested published packages. Configure the same MIME types on external hosts.

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

## Animated 3D fox showcase

`projects/FoxShowcase` is a Rust `webgpu-3d-v1` third-person sunset world.
A continuous 160-by-160 landscape connects the fox meadow and ruined sanctuary
to forests, rocky mountain trails and a western beach. The fox, two deer and
two wolves travel through their habitats and alternate walking with resting
animations. The Mixamo humanoid uses idle/walk animation, a capsule character
controller, jumping and a directly controlled orbit camera without trailing
position interpolation or automatic recenter. The low amber sun, muted horizon,
restrained local lights and reduced exposure/bloom keep the lighting subdued.

The environment includes water, molten/plasma, gem/sheen and object-lit
materials, baked/vertex-colour surfaces, scrolling textures,
wind foliage, a material graph, light shafts and 3D particles. Directional,
point and spot shadows combine with physical sky/clouds, height fog, darkness,
GTAO, SSR, volumetrics, bloom, auto exposure, DOF, camera motion blur,
grading/lens effects, FXAA and sharpening.

Click the game for keyboard focus. WASD walks, Shift moves briskly, Space jumps,
left-drag orbits, the wheel zooms and R respawns. E opens nearby field notes;
Escape closes them and restores movement. Discover the trailhead, pond, sanctuary,
woodland shrine and coastal lookout to complete the saved journal. The HUD uses
an engraved sunset crest flush to the top-left corner and five stateful discovery
stamps. The travel controls sit flush to the bottom-left; the illustrated journal
hint sits flush to the bottom-right. Both retain internal text padding. Field
notes open a leather-bound journal with parchment pages and a world map whose
exposed beach footprint is projected from the actual scene geometry; its slider
and switch are real pointer/keyboard-operated 2D UI controls. At the sanctuary,
F launches a stone toward the supported hinge relic; G removes the projectile
and resets the resting stone. Gameplay continues while the journal is open,
but player movement is blocked.

Fox attribution and the Kenney/Quaternius CC0 notices are packaged in
`assets/credits.txt` and `assets/licenses/`. Source scene includes separate
landscape, wildlife, ground-cover, player, terrain-collision and interaction scenes.

Publish through the normal Web pipeline with `-Project projects/FoxShowcase`.
The development gallery at `http://127.0.0.1:18989/` lists the project and
serves its published package at `/projects/FoxShowcase/`. Registered project
routes cannot escape their package roots; unknown projects/files return 404.
The ZIP is `projects/FoxShowcase/Builds/Web/FoxShowcase/FoxShowcase.zip`.
This combined artistic map is not an additional native/WebGPU cross-product
parity guarantee.

## Built-in 3D grass

Add the reflected **Grass** component in the native Inspector, or author
`[entities.grass]` in a 3D scene. The same descriptor is consumed by native and
Web runtimes; a project-specific grass script or prebuilt patch models are not
required. Optional `surface_mesh` and `exclusion_mesh` models are sampled in
the grass entity's local space; no surface means a flat field at local Y=0.
Source node transforms are applied before sampling. Exclusions prevent blades
on paths and paving; missing source surface triangles remain holes.

```toml
[[entities]]
name = "Meadow"
[entities.grass]
surface_mesh = "project://assets/models/terrain.gltf"
exclusion_mesh = "project://assets/models/paths.gltf"
region_min = [-50.0, -50.0]
region_max = [50.0, 50.0]
spacing = 0.14
chunk_size = 4.0
height_range = [0.35, 0.85]
blade_width = 0.035
lod_distances = [10.0, 26.0]
draw_distance = 44.0
wind_strength = 1.0
seed = 1
```

Spacing, chunk size, height and width use local-space metres. LOD and draw
distances use world-space metres. Camera-local chunks contain deterministic,
curved, opaque blades with vertex-tinted roots and tips. Three disjoint density
layers fade gradually instead of replacing whole patches. Chunks retain
ordinary camera-frustum culling, conservative wind bounds and terrain materials
under the distant fade. Prepared surface/exclusion data and unchanged chunks
are reused; outgoing GPU resources retire only after submitted work completes.

Set `surface_mask = true` to paint growth through the surface model's vertex
colours: red controls density, green scales height, blue blends toward dry gold.
Keep vertex-colour rendering disabled on the terrain material when those colours
are growth data rather than visible terrain tint. `base_color` and `tip_color`
set the meadow palette. Wind strength changes material parameters without
regenerating geometry.

Add **Grass Bender** to the player or another moving entity:

```toml
[entities.grass_bender]
radius = 0.9
strength = 0.8
```

The nearest eight enabled benders per view bend nearby blades away with smooth
radial falloff; roots remain planted. Wind and bending use the same displacement
in colour, depth and shadow passes. The browser's 64 MiB resident mesh budget and
1,024 mesh-slot limit include ordinary models as well as grass. Tune density and
view distances for the rest of the scene; scene changes release old model,
primitive and grass GPU residency rather than retaining historical worlds.
