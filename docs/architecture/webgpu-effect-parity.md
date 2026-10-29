# WebGPU effect parity inventory

This ledger describes the native source contract and bounded browser implementation.
A row is qualified only by a maintained semantic case and actual native/WebGPU
captures; sharing source, compiling WGSL, or retaining an authored field is
insufficient. Task 4 and earlier checkpoints remain historical evidence.

Production supports `webgpu-3d-v1` alongside the default `webgpu-2d-v1`; support
does not qualify arbitrary 3D content, settings or combinations.

## Evidence and status

- **Qualified**: the bounded configuration has the recorded Task 3, 4 or 6 evidence below.
- **Shared, unqualified**: common equations exist, but the feature lacks a recorded
  native/browser semantic comparison.
- **Implemented, unqualified**: the adapter exists, but the stated case has no final recorded comparison.
- **Missing**: browser scheduling, resource access, simulation or shading is absent.
- **Native tooling**: editor-only rendering outside the exported game's scope.
- **Infrastructure**: implementation differs by backend; equivalent observable
  output and correct ownership must be tested rather than matching dispatches.

Task 3 evidence is recorded in the implementation plan and the local, untracked
`logs/task3/qualified-web/report.json`, `qualified-web/browser.json`,
`qualified-native/report.json`, `qualified-parity.json`, and
`qualified-languages/report.json`. The checkpoint records 56 hardware browser
cases and 21 same-scene comparisons, including six shadow comparisons, plus the
native and both-language regressions. These measurements do not qualify any
Task 4 effect added after that checkpoint.

## Task 6 production export qualification

The Rust and C# package parity reports are
`.temp/task6/final-parity-rust-2/report.json` and
`.temp/task6/final-parity-csharp/report.json`. Both passed. Their sibling
`gameplay-parity.json` reports compare named-entity world transforms and script
properties, not pixels, for BrowserPhysics3D, BrowserBodies3D, BrowserRaycast3D,
BrowserContacts3D, BrowserCharacter3D, BrowserRagdoll3D, BrowserSkinClips3D,
BrowserPrefab3D and BrowserTransition3D at ticks 1, 10, 30, 60, 120 and 150.
The inclusive absolute tolerance is 1e-5, including canonical quaternion
rotation. Their `lighting-parity.json` reports compare 21 selected scenes in both
languages and two additional custom-pass scenes in Rust only, at 512x512 and
tick 10. Whole-image MAE 8, p95 18 and probe 12 limits are unchanged. These
fixtures do not qualify all material, lighting, effect or gameplay combinations.

Parity uses an instrumented bridge-enabled sibling, not the production
executable. The reports verify byte-identical game and engine PAKs and
publish-time gameplay input hashes; executable bytes differ by instrumentation.
Separately, bridge-free relocated production packages passed startup, nested-path
gameplay, local iframe gesture/gameplay, manifest and production boundary checks,
and unavailable-WebGPU diagnostics. Evidence is in each parity directory's
`production-verification.json` and `.temp/task6/final-publish-rust/` and
`.temp/task6/final-publish-csharp/` `web-verification.json` files. These production
checks are not fixed-tick native parity. `logs/task6/publish-rust.json` and
`logs/task6/publish-csharp.json` record used features and unqualified ranges.

The production measurements used Windows 10.0.26200, hardware NVIDIA Blackwell
and Chrome 156.0.8078.48. `tools/browser-tests/distribution-budgets-3d.json`
records the samples and fixture guardrails:

| Metric | Rust | C# | Fixture maximum |
|---|---:|---:|---:|
| ZIP bytes | 3,663,981 | 4,720,555 | 9,500,000 |
| Cold interactive ms | 2,274.1 | 2,199.1 | 4,600 |
| Warm interactive ms | 2,053.3 | 2,257.3 | 4,600 |
| Wasm linear memory bytes | 338,427,904 | 359,596,032 | 720,000,000 |
| Frame-interval p95 ms | 16.7 | 16.8 | 34 |

These are single local fixture samples, not universal performance claims;
no edited-publish budget is asserted.

The Task 6 evidence alone does not qualify portable custom passes outside its
Rust 2D fixture, arbitrary material/effect combinations, other devices or external
hosts. The parity expansion below records later measurements separately.
Arbitrary Slang, raw SPIR-V and desktop-only services remain outside the portable
contract; networking uses an explicit WebSocket gateway rather than browser UDP.

## Rendering and runtime parity expansion

The 29 October 2026 integration extends the native contract rather than changing
the image limits. Qualification is still case-specific:

- `logs/parity-full/samplers-fixed.json` records passing anisotropy 16 versus 1,
  LOD bias 2 versus 0, and bias reset to zero. Its aggregate failed the then-invalid
  specular control; `logs/parity-full/specular-qualified.json` subsequently passes
  the separate specular-filter enabled/control case. Native loose/cooked and Web
  textures now use the same alpha-weighted, colour-space-aware mip construction.
  Native cooked data textures have separate linear variants and DDS loading
  uploads the complete mip chain.
- `.temp/lighting-capacity/report7.json` records eight passing 512x512 native/Web
  captures: 129 distributed lights with off-axis perspective/orthographic controls,
  a visible 17th spot-shadow map and the eight-versus-nine-point 48-entry budget.
  `.temp/lighting-capacity/oracle-rerun.json` exercises the added semantic ROI
  comparison on those recorded images. These do not qualify per-tile overflow.
- `.temp/lighting-capacity/farfade1.json` records directional shadow receiver
  controls at view depths 250, 300 and 330. Native red-channel shadow deltas are
  48.92, 16.76 and 0; Web deltas are 48.84, 17.80 and 0. The maximum semantic
  probe error is 1.12 under the unchanged limit 12. Global MAE/p95 pass despite
  an individual sampled-channel error of 87; this is not a maximum-pixel guarantee.
- `logs/parity-full/material-cross-qualified.json` passes seven mirrored,
  nonuniform-scale, alpha-blend/cutout and animated-skin captures plus three
  independent semantic controls. Per-scene MAE is at most 0.227 and p95 is 1;
  the existing maximum-pixel differences are not hidden or used as a new limit.
- `logs/parity-full/temporal-transition-probes.json` qualifies the first accepted
  camera-cut and scene-switch frame at tick 10. Motion probes change on both
  backends while adapted exposure survives. A real frozen tick-9 capture and a
  deliberately rejected tick-10 capture leave the final image and exposure
  unchanged on retry.
- `logs/parity-full/runtime-api-smoke.json` records actual C# animation import,
  pending-clear and compile behavior on native and hardware WebGPU. Pending clips
  are animator-local; compilation extends the shared model catalog as on native.
  The Web smoke also exercises joint offsets and all four master/physics-debug
  toggle combinations. Debug geometry remains at the same output-pixel positions
  with 512x512 output and 256x256 scene depth; disabled overlays contribute no
  diagnostic-colour pixels. This is a runtime/semantic smoke, not a whole-image
  native debug comparison.
- `.temp/data-mip-pair-report.json` passes the alpha-varied versus opaque linear
  data texture pair. Each backend's receiver ROI is unchanged (MAD 0, limit 1);
  native/Web MAE is 1.159, p95 is 9 and maximum probe error is 2.72. Alpha no
  longer weights or dilates unrelated linear data channels.

Networking uses a bounded WebSocket-to-ENet gateway with shared replication/RPC,
ownership and application-route ping. Final hierarchy evidence is
`.temp/network-hierarchy-client-connected.json`,
`.temp/network-hierarchy-browser-host.json` and native jobs
`logs/gameplay-jobs/c6e6765f-6eb1-42f5-84ec-c5c1076f24e5/` and
`logs/gameplay-jobs/ee07fe55-d3ae-4449-93b8-823bf954a75c/`.
Both initial-attachment root and child consume payload 99, report local
ownership/authority and cannot send an RPC. Both host directions exchange
actors, fields, RPCs and real application-route ping. Host goodbye reaches the
opposite client before cleanup, leaving only the local predicted hierarchy.
`.temp/network-gateway-admission-evidence.json` also rejects an allowlisted
alias requested with the wrong port (1008), invalid tokens and disallowed origins.
These are localhost paths, not public deployment or native LAN/TURN services.

`.temp/input-parity-proof.json` records real pointer-lock acquisition/movement
and Escape release on hardware Chrome, plus managed async Unicode clipboard
access and permission denial in an isolated headless-shell clipboard.
Physical rumble is unqualified because no controller was connected. The earlier
UI copy/paste check used unified headless Chrome without preserving the owner's
OS clipboard; it may have overwritten it. Subsequent checks do not use that
unsafe procedure.

Packaged C# enumeration matches seven native PAK outcomes, including nested
matches, invalid/traversal paths, engine enumeration and read-only writes:
`.temp/packaged-glob-native/report.json` and
`.temp/packaged-glob-native/browser-report.json`.

`logs/parity-full/audio-runtime-proof.json` measures actual post-mixer
AudioWorklet output for gain, pitch, looping, buses, music fade/interruption,
cone/distance attenuation, WAV/MP3/FLAC and Audio Source playback. It is digital
signal evidence, not physical speaker/device-latency qualification.

`.temp/ui-parity-restaged.json` passes five 512x512 native/Web captures with
FXAA enabled and unchanged MAE 8 / p95 18 / probe 12 limits:

| UI fixture | MAE | p95 |
| --- | ---: | ---: |
| Rounded widgets, nested masks and text boxes | 0.114 | 1 |
| Native UI effects, image and glyph materials | 0.121 | 1 |
| Shader control | 0.131 | 1 |
| Native font curves, Latin-1 and private-use icons | 0.056 | 1 |
| Solid background | 0.146 | 1 |

Original UI shader sources are compiled to native SPIR-V in the private capture
overlay and to role-specific WGSL; a missing native shader is not a valid control.
`.temp/ui-fxaa-order-control.json` records zero changed UI ROI channels between
FXAA on/off on each backend. `.temp/ui-parity-half-scale-report.json` passes
512x512 output with 256x256 scene rendering (MAE 0.105, p95 1). Trusted typing,
non-monospace selection and mouse-positioned insertion are recorded separately.

Final source-current gates:

- `logs/parity-full/native-debug-gauntlet.json`: Debug Editor/GameRuntime build,
  1,607/1,607 native cases and both Vulkan validation smokes pass with zero
  validation errors, warnings or advisories. The earlier Debug failure exposed
  a missing `/bigobj` option on `RuntimeFramework`; the failed receipt is retained.
- `logs/parity-full/browser-gauntlet.json`: hardware Chrome automation and
  bridge-free production-boundary gauntlet passes; its browser/server/child
  cleanup is recorded. This is not a cross-browser result.
- Basic Rust/C# production exports pass relocated ZIP/iframe verification:
  jobs `4cf8ddfd-467a-483b-a0a1-0bf8cd6a8bfd` and
  `79c91bc0-9879-493a-bc17-5ee5b2592d50`. Initial verifier failures were missing
  the pinned Playwright module; reruns supply it in the child environment.
- Real Rust 3D production and package-sibling qualification pass:
  `logs/parity-full/rust3d/publish.json` and
  `logs/parity-full/rust3d/retry/parity.json`. Physics, bodies, raycast and
  contact traces match at ticks 1/10/30/60/120/150 to 1e-5 and detect 1e-3 drift.
- C# 3D production and package-sibling qualification pass:
  `logs/parity-full/csharp3d/publish-final-retry.json` and
  `logs/parity-full/csharp3d/package-parity.json`. Bodies, contacts, raycast and
  character traces use the same checkpoints/tolerance and perturbation control.
  Both languages' production/sibling PAKs and gameplay input hashes match;
  the bridge-enabled executables are not claimed identical to production.
- `logs/parity-full/audio-motion-proof.json` adds actual moving-emitter Doppler
  and camera-listener translation output checks. The static/audio-motion cases
  do not qualify arbitrary motion, physical speakers or latency.
- Final Python suites pass: project 80 (6 skipped), web-runtime 104 and shaders
  28. Browser verifier unit checks pass 10/10. clangd configuration and the
  graphify code graph are refreshed.

The C# 3D run exposed static asset enumeration/write destinations being mistaken
for read dependencies. The publisher now treats `Assets.List` patterns and
`Assets.WriteText` destinations as queries/output paths; real read references
remain validated and enumerated files still need normal reachability/declarations.
Transient sccache connection failures and initial fixture/cook failures remain
in their original reports rather than being reclassified as passes.

These receipts qualify the tested combined working tree. The owner selected
separate commit ownership: isolated parity changes are checkpointed, while mixed
ABI2/streaming integration hunks remain unstaged for the concurrent work.
Historical Task 6 measurements are not substituted for these new runs.

### Prerequisite source checkpoint

The curve-UI wiring is also exercised independently of uncommitted streaming
implementation: a source export of `c85ec677` plus the isolated UI prerequisite
blobs compiles, passes the Wasm import/source-closure audits, and runs
`BrowserUiParity`, `BrowserUiShaders` and `BrowserFontParity` on hardware Chrome.
`logs/parity-prerequisites/isolated-ui-smoke.json` records trusted startup,
512x512 captures, empty runtime diagnostics and clean stop for all three scenes.
This is source-isolation smoke proof, not another native pixel-comparison run.

`Run-WebRuntimeChecks.ps1` now requires the supported `audio-source` case rather
than the removed invalid-component case; the actual script passes all 72 cases
in `logs/parity-prerequisites/browser/report.json`. The native ownership test
explicitly selects fixed-delay interpolation without extrapolation: its prior
behind-the-host assertion was incompatible with the default prediction policy.
Production extrapolation defaults/budgets are unchanged. Both targeted ownership
and re-entrant attach cases, followed by the full native suite, pass; see
`logs/parity-prerequisites/native-unit.xml` and `verification.json`.


## Task 4 bounded post qualification

`logs/task4/qualified-post-summary.json` records **30 passed post controls** at
512x512 output. It merges selected successful cases from four incremental runs;
source reports, superseded failures, fixture/package hashes and selected capture
receipts remain in the summary. This is not a claim that one unchanged binary
ran all 30 in one batch, or that all combinations of effects match.

The maintained `post-parity-checks.py` cases qualify contrast 1.6, saturation 0,
temperature/tint 0.7, vignette 0.9, chromatic aberration 8, grain 1, bloom 1,
FXAA enabled, sharpness 1 and renderScale 0.5 against neutral controls. The same
resolve controls cover display-space particles. GTAO uses the authored corner,
SSR the coloured objects and reflective floor, and volumetrics the authored
participating-air scene. Other parameters use the recorded native defaults.
Motion blur 1/maxRadius 32 captures the final moving camera tick. Auto exposure
uses exact HDR 2, key 0.18/speed 2, analytic three-slot/six-frame adaptation and
frozen-frame receipts. Eleven tonemap operators match eight independent emitted
radiance bands, including a neutral ACES control and non-ACES fallback rejection.
The analytic meter materials use black base colour and baked lighting to exclude
unmetered procedural sky specular radiance.

Both backends must pass independent enabled/control behavior and the existing
whole-image limits (MAE 8, p95 18), with probe limit 12. Tone analytic channel
limit 3 remains unchanged. The shared FXAA contrast-side sign and grain integer
hash corrections are covered by actual corrected captures. FXAA changed 43
probes (31 in its edge region) per backend; display FXAA changed 13 edge probes.

Parameter sweeps compare a non-default value against the same effect at its native
default (`logs/task4/rerun/sweeps.json`; GTAO radius `sweeps-2.json`, vignette
roundness `sweeps-4.json`): vignette
roundness 0 on a 640x384 frame brightens the sides and leaves the vertical falloff
alone; bloom radius 3 spreads farther; GTAO radius 1.5 darkens only (60/65 contact
pixels native/Web, none lighter) and strength 2 darkens the probes; reflection
intensity .3 and max roughness .06 remove floor radiance; exposure key .36 and
speed 6 match the analytic meter. Cross-products `grade-lens-stack` (contrast,
saturation, vignette, chromatic, grain, bloom, FXAA, sharpen) and `scene-stack`
(GTAO, SSR, volumetrics, bloom) pass the unchanged whole-image limits.

Transitions (`logs/task4/rerun/transitions.json`) apply settings or a window/canvas
resize at a fixed tick on both backends (`lighting-parity-checks.py --changes-json`,
native `launchConfig.changes`; a change at tick N lands before update N+1). GTAO and
SSR enabled at tick 9, motion blur enabled at tick 8 and bloom/motion blur resized
from 384x320 at tick 5 equal the steady-state capture exactly (MAE 0.0) on both
backends. Enabling metering at tick 4 or resizing at tick 5 restarts exposure from
manual 1 identically on both backends (0.92451 against steady 0.85691), and
disabling it at tick 6 returns manual exposure 1 and the neutral image. Resize
targets follow `--extent`; the three resize transitions also pass at the odd
513x511 extent (`logs/task4/rerun/transitions-odd.json`).

The later expansion above qualifies the recorded specular, minified sampler and
first-frame camera/scene-history cases. Arbitrary adversarial combinations remain
separate gates; implementation range checks do not qualify those ranges.
The Web effect planner rejects non-finite settings, invalid scale/radius/exposure
values, source/output extents above 4096 and estimated transient effects above
512 MiB before allocation. Project custom passes are supported only through the
portable `CustomPass.slangh` contract below; arbitrary authored Slang shader
capability remains unsupported, and supported material-graph lowering is a separate
bounded adapter, not a general shader compiler guarantee.

The Task 4 checkpoint preceded the Task 6 production 3D publishing gate above.

## Task 4 material and scene-effect qualification

`logs/task4/rerun/materials-3.json` (19 cases, 9 pairs) and
`logs/task4/rerun/scene-effects.json` (22 cases, 10 pairs) pass on hardware
Chrome/NVIDIA Blackwell at 512x512, tick 10, exposure 1, with the unchanged
whole-image limits. Per-scene MAE is 0.105-0.177 for materials and 0.15-0.885
for scene effects; the worst p95 is 5. `logs/task4/rerun/negative-visual.json`
re-proves on these captures that 13 deliberate regressions fail their gates.

The first final run failed molten and physical sky. Both were shared-code
defects, not adapter differences:

- `ProceduralHash31` took `frac` of a product near 2.5e5, where float32 keeps
  1/64 precision. Native SPIR-V and WGSL fused or ordered its multiplies
  differently, so roughly 0.07% of lattice corners wrapped and whole noise cells
  flipped. A CPU reference matched Web (rank correlation 0.973) better than
  native (0.906). It is now an integer PCG3D hash, bit-identical on every
  backend and driver; molten MAE fell from 1.70 to 0.15. Molten's crack layout
  changes everywhere, including native projects.
- Slang 2026.8's WGSL backend emits a negated vector sum `-(a + b)` as
  `vec3<f32>(0) - a + b`. `PhysicalSky`'s extinction terms flipped the Mie sign,
  driving red negative on Web (native was correct). The terms are now one
  product, and `tools/shaders/wgsl_checks.py` fails the build and the material
  cook on any ambiguous zero-minus vector expression.

Fixture/oracle corrections, each keeping the image tolerances unchanged:
the physical-sky pair uses the engine's daylight sun intensity 5.2 (at 1.5 the
correct sky was below the visibility probe on both backends); the uniform
seabed in the three water fixtures is 40 units wide, because at 20 the deep
variant moved it out of the pair's own unchanged-corner control; the molten
probe moved to a lit cell of the new deterministic pattern; and the foliage
pair's minimum difference is 0.1. At tick 10 the wind moves the card tip by
about 4 pixels, measured identically on both backends (0.182 native, 0.183 Web,
307 changed pixels); the previous 0.25 never passed in any recorded run, while
an implementation that ignores the foliage flag produces 0.

Regression gates on the same tree: the full Debug native gauntlet
(`logs/task4/rerun/native-gauntlet-2.json`) passes 1,549 unit cases with zero
editor/runtime Vulkan validation findings, and both C# and Rust pass production
2D publishing plus private 3D movement (`logs/task4/rerun/languages/report.json`).
The first Debug gauntlet exposed a latent native crash: since children are
destroyed with their parent, `RestoreSceneInPlace` (Play/Stop) destroyed a scene
entity a script had parented under a runtime-spawned one, then re-applied the
snapshot onto the dead handle. Restore targets are now detached first. Earlier
gauntlets ran RelWithDebInfo, where EnTT's validity assert is compiled out.

## Native pass inventory

Parameterized names represent every generated level, cascade, target or preview.
The authoritative runtime ordering is
`src/engine/rendering/RenderingSubsystem.cpp::RegisterPasses`; the post stack is
`src/engine/passes/PostProcessStack.cpp::RegisterPasses`.

| Native pass or operation | Source / shader | Browser implementation / evidence | Required maintained parity case |
|---|---|---|---|
| Main draw culling; `$CullDraws_Shadow`; `$CullLocalShadowDraws` | `CullPass.cpp`, `ShadowService.cpp`, `LocalShadowService.cpp`; `cull_draws*.slang` | Infrastructure: browser explicit bounded draws | Visibility, disabled ancestors, winding, bounds and draw-limit rejection; no dropped valid draw |
| Pose init, clip sampling, node flatten, skin palette build | `RenderQueue.cpp`; `pose_init`, `animation_sample`, `node_flatten`, `skin_palette_build` | Infrastructure: portable CPU pose extraction, qualified limited fixtures | Fixed pose and interpolation, multiple primitives, shadow pose; full gameplay animation remains Task 5 |
| `$ScenePreDepth` | `scene_prepass.slang`, `DepthVertex.slangh` | Implemented auxiliary normal/base-colour/depth prepass; bounded GTAO/SSR post cases qualified | Same depth, octahedral normal and base colour at opaque/cutout/skinned edges |
| `$DirectionalShadow_Cn`, `$ShadowDepthTransition` | `ShadowService.cpp`, `shadow_depth.slang` | Qualified bounded three-cascade path and 250/300/330 far-fade controls | Existing Sun/Mask/Skin/NoCast fixtures; other cascade boundaries remain unqualified |
| `$LocalShadowAtlasRender` | `LocalShadowService.cpp`, `local_shadow_depth.slang` | Qualified bounded point/spot shadows, visible 17th spot map and eight-versus-nine-point 48-entry selection | Shared native map resolutions and dynamic shelf packing; broader scene distributions remain unqualified |
| `$Lighting` tiled light list compute | `LightingManager.cpp`, `tiled_light_cull.slang` | Infrastructure: Web compute tile lists with the native 128-per-tile bound; qualified 129 globally distributed lights | Off-axis perspective/orthographic controls; per-tile overflow and arbitrary distributions remain unqualified |
| `$GTAO_Main`, `$GTAO_Denoise` | `GTAOPass.cpp`; `gtao_main`, `gtao_denoise` | Qualified bounded ambient-corner on/off case, radius 1.5 and strength 2 sweeps, tick-9 enable transition; material texture AO is separate | Ambient-only corners, denoise edge preservation and empty-3D reset |
| `$Skybox` | `skybox.slang`, `Sky.slangh` | Qualified Task 3 gradient; physical sky and cloud on/off pairs qualified by the Task 4 scene-effect suite | Turbidity/sun-elevation sweeps, solar disk/stars and cloud speed over time remain unqualified |
| `$EngineForward` | `gltf_mesh.slang`, shared includes | Qualified Task 3 standard PBR subset; specialized variants, contact shadows, height fog and darkness volumes qualified by bounded Task 4 pairs below | Cross-products of specialized variants and the unqualified rows below |
| `$SSR`, `$SSRComposite` | `ssr.slang`, `ssr_composite.slang` | Qualified coloured-object reflective-floor on/off case, intensity .3 and max-roughness .06 sweeps, tick-9 enable transition | Missing hit fallback, depth discontinuity and camera movement remain unqualified |
| `$VolumetricFog`, `$VolumetricComposite` | `volumetric_fog.slang`, `volumetric_composite.slang` | Qualified bounded participating-air on/off case | Sun-blocked beam, extinction and half-resolution bilateral edge upsample sweeps remain unqualified |
| `$DepthOfField` | `dof.slang`; camera lens extraction in `AetherCore.cpp` | Qualified bounded DofOn/DofOff same-scene pair | Focus plane stays sharp, near/far CoC sign and size, aperture/focal-length response, orthographic disable; preserve native insertion/composition ordering |
| `$BillboardParticles` | `BillboardParticleRenderer.cpp`, `particle_billboard.slang` | Qualified fixed-seed 2D/3D bursts and display-only burst | UV/rotation/lifetime key sweeps, alpha/additive mixes, read-only depth, lit smoke, fog and HDR ordering remain unqualified |
| `$CustomPassBehind2D` | `CustomPassRenderer.cpp`, `WebCustomPassRenderer.cpp`, `include/CustomPass.slangh` | Implemented on both backends through the portable `CustomPass.slangh` contract (see below); qualified by `BrowserCustomPass`/`BrowserCustomPassOff` (logs/task4/custompass/report.json: native/Web image MAE 0.07, all ordering checks true on both backends): Behind rectangle hidden by the sprite it overlaps, uncovered part drawn, min-x/min-y border orientation | Time/animation-driven passes, 3D scenes, non-HDR legacy path; the 2D publisher rejects project `.slang` shaders, while the 3D profile cooks portable custom passes |
| `$Renderer2D` sprite/tile pass | `Renderer2D.cpp`, `sprite2d.slang` | Qualified sprite/tile subset | Particle Plane2D extraction and ordering; mixed HDR/display-space composition |
| `$CustomPassOver2D` | `CustomPassRenderer.cpp`, `WebCustomPassRenderer.cpp`, `include/CustomPass.slangh` | Qualified by `BrowserCustomPass`/`BrowserCustomPassOff` on both backends: Over rectangle equals the lit white sprite at the lamp-mirrored position, darkens away from the lamp, border orientation | Same as `$CustomPassBehind2D` |
| `$Light2DOccluders`, `$Light2DComposite` | `Light2DCompositor.cpp`; `occluder2d`, `light2d` | Qualified tile-art alpha, shared native light equations | Transient capsule submissions, shadow softness and 2D medium shafts/noise; current reconstruction is bounded to orthographic cameras |
| `$CustomPassEmissive2D` | `CustomPassRenderer.cpp`, `WebCustomPassRenderer.cpp`, `include/CustomPass.slangh` | Qualified by `BrowserCustomPass`/`BrowserCustomPassOff` on both backends: emissive rectangles render identically on the lamp and outside its radius (light map never touches them), full brightness on a 0.02 ambient, border orientation | Same as `$CustomPassBehind2D`; absolute emissive colours are compared relationally, not against a tonemap model |
| `$BloomDownN`, `$BloomUpN` | `PostProcessStack.cpp`, `bloom.slang` | Qualified bounded HDR highlight/spread control, radius 3 sweep and 384x320-to-512x512 resize transition | Per-level dimensions, odd extents and zero-strength identity |
| `$HistogramClear`, `$Histogram` and exposure adaptation | `PostProcessStack.cpp`, `luminance_histogram.slang` | Qualified analytic HDR 2 adaptation, frozen capture, key .36/speed 6 sweeps, and enable/disable/resize transitions (late enable and resize restart from manual 1 on both backends) | Dark adaptation, zero pixels and camera/scene-change history |
| `$PostProcess` | `tonemap.slang` | Qualified 11 operators and bounded grade/lens/grain/motion controls; see settings scope below | All operators, bloom/DOF blend, grade, lens effects, grain, motion blur and sharpening; see post-settings table |
| `$BillboardParticlesDisplay` | after-tonemap callback in `RenderingSubsystem.cpp` | Implemented before final resolve; qualified display-particle FXAA/sharpen/render-scale controls | Gamma/display-space alpha/additive, depth and mixed ordering combinations remain unqualified |
| `$FXAA` / final resolve | `PostProcessStack.cpp`, `fxaa.slang` | Qualified diagonal/display-particle controls and bounded scaled output | Odd extents, resize and mixed UI ordering coverage remain separate gates |
| `$Debug` physics/debug geometry | `PhysicsDebugRenderer`, `DebugLayer`, `WebDebugRenderer` | Runtime-qualified C# immediate geometry and collider overlay; shared toggles/shape extraction | Four master/physics toggle states, correct clip-Y orientation and scaled-depth path; native/Web whole-image comparison remains separate |
| `$UiOverlay` shape draw building, shapes and text | `UIRenderer`; `ui_build_draws`, `ui_shapes`, text shader includes | Qualified bounded browser UI subset; separate browser extraction | Post-tonemap display colour, text/atlas bounds, clipping/ordering and input regressions |
| `$CameraRT_<id>` and its cull | `RenderTargetService.cpp` | Not reachable from exported games: `CreateCameraRenderTarget` has no caller and no C#, Rust, interop, scene or reflected field creates a target, so no game can request one on either backend | Port together with whatever first exposes it to gameplay: multiple target extents, camera state, texture feedback prohibition, resize and target retirement |
| Camera/model/material-thumbnail preview passes and ready transitions | `CameraPreviewService.cpp`, `ModelPreviewService.cpp`, `MaterialThumbnailBaker` | Native tooling | Keep native smoke checks; not required to ship an editor inside the browser |
| `$TexturePreview`, `$TexturePreviewReady` | `texture_preview.slang` | Native tooling | Native diagnostics unchanged |
| `$UiShellClear`, viewport clear/ready, ImGui overlay | `RenderingSubsystem.cpp`, application overlay | Native tooling / infrastructure | Full and UiShell initialization/shutdown and native validation smokes |

## Every authored material switch and shader family

`MaterialAsset.hpp`, `MaterialSystem.cpp`, `GpuMaterial.slangh`, and
`gltf_mesh.slang` define the switches. Combinations matter: a successful test of
one switch does not qualify every cross-product. Each effect case must include
its intended opaque/cutout/blended, static/skinned and shadow interactions.

| Authored variant | Browser implementation / evidence | Required parity case |
|---|---|---|
| Standard metallic/roughness PBR; albedo/normal/MR/AO/emission maps | Qualified bounded fixtures | Preserve Task 3 map, colour-space and direct-light probes |
| `doubleSided`, mirrored transform and tangent handedness | Qualified bounded fixtures | Preserve front/back and mirrored culling probes with normal maps |
| `alphaMask`, `alphaBlend`, `additiveBlend` | Qualified bounded fixtures | Preserve alpha cutout/shadow, back-to-front blend and HDR additive checks |
| `modulateVertexColor`, UV scroll | Qualified VertexColor/VertexColorOff (per-corner hues on, ignored off) and UvScroll/UvScrollOff (one-band shift at tick 10) pairs | Non-uniform scroll speeds and vertex-colour on baked/specialized variants |
| `receiveShadows`, MeshRenderer visible/cast-shadows | Qualified bounded fixtures | Keep independent visible/no-cast and receive-shadow oracles |
| `foliage` wind and caster offset | Qualified Foliage/FoliageOff pair at tick 10 (see threshold note) | Same world-space wind in forward/prepass/shadows at a fixed elapsed time |
| `bakedLighting` | Qualified Baked/BakedOff pair | Prelit vertex colour keeps bake, cascade shadows only darken to native shade floor; no accidental PBR/AO |
| Foliage+baked+masked soft edge copy | Qualified through the foliage fixture (baked, alpha-masked, foliage) | Opaque depth/cutout core plus blended fringe; fringe does not cast or repeat skin draws |
| `sky` mesh | Qualified SkyMesh/SkyMeshOff: a dome nearer than an opaque cube is pinned behind it as sky and occludes it otherwise | Multi-layer domes and primitive ordering between sky layers |
| `lightShaft` blended cards | Qualified LightShaft/LightShaftOff pair | Fixed-time shimmer, soft alpha, depth intersection and authored blend mode |
| `objectLit` | Qualified ObjectLights/ObjectLightsOff pair | Ambient plus two native object light records without substituted PBR lights |
| `gem` | Qualified Gem/GemOff pair | Facet normal, fresnel rim, interior fake refraction, sparkle and emissive overrides |
| `sheen` | Qualified Sheen/SheenOff pair | Object-lit pickup rim/highlight and correct blended interaction |
| `water` | Qualified Water/WaterOff shading and Water/WaterDeep depth-tint pairs | Shared ripple/depth tint, screen reflection with real prepass inputs, sun glints and native fallback |
| `character` | Qualified Character/CharacterOff pair | Wrapped key, rim, eye catchlight/texture relief and character blob contact core |
| Built-in `plasma` and `molten` effects | Qualified Plasma and Molten intensity pairs; molten noise is now bit-identical across backends | EffectParams tint/speed/scale/intensity at identical tick, intended pipeline/depth/blend modes |
| Authored `templateDesc.shaderVfsPath` / `fragmentVfsPath` | Supported bounded generated material shader adapter; raw arbitrary Slang remains unsupported | Cook supported custom Slang to WGSL; attribute/binding validation and precise diagnostics for forbidden capabilities |
| Material-graph generated shaders | Qualified BrowserGraph and Graph backface pair through the shared graph lowering/WGSL cook | Shared graph lowering, parameter/texture binding, reproducible shader hashes; deliberately unsupported graph node produces actionable cook diagnostic |

## Post settings and extraction ownership

Native settings enter `Renderer` / `PostProcessStack` through the settings service;
camera and scene content enter the owned `RenderFramePacket` in `AetherCore.cpp`.
Browser project manifests carry the native `graphics` table into the full
implemented post-settings snapshot. Qualification remains bounded to the
recorded configurations above.

| Input | Exact native extraction / equation | Implementation and qualification boundary |
|---|---|---|
| Grade contrast/saturation/temperature/tint, vignette intensity/roundness | `tonemap.slang::ApplyGrade`, post-stack push constants | Implemented shared equations; bounded grade and vignette intensity controls qualified; roundness sweep/background combinations unqualified |
| Bloom strength/filter radius | `PostProcessStack.cpp`, `bloom.slang` | Implemented bounded pyramid; highlight-spread case qualified; radius sweep and every mip/odd-size combination unqualified |
| Auto exposure enable/key/speed | histogram and adaptation in `PostProcessStack.cpp` | Implemented GPU histogram ring; analytic HDR 2/default-key/default-speed cadence qualified; dark/adaptive sweeps and resize/toggle transitions unqualified |
| Chromatic aberration, sharpness, film grain | `tonemap.slang` | Implemented shared composition; bounded chromatic/sharpen/grain controls qualified; arbitrary parameter combinations unqualified |
| Motion-blur strength/max-radius | `tonemap.slang`, previous view-projection in frame constants | Implemented accepted-frame camera history; moving-camera control qualified; native history persists across scene/camera changes, and transition combinations remain unqualified |
| FXAA, render scale | final resolve in `PostProcessStack.cpp` | Implemented source/output split, native floor/clamp scaling and final resolve; bounded mesh/display-particle controls qualified |
| Anisotropy and mip LOD bias | `EngineSettings::Graphics`, native sampler creation and material sampling | Qualified minified angled 16-versus-1 anisotropy, bias 2-versus-0 and reset-to-zero controls; other values and devices remain unqualified |
| Sky/fog | `DayNightSystem::ApplyStaticSky`; `SkyComponent`; `Sky.slangh`, `HeightFog.slangh` | Shared extraction; fog, clouds and physical-sky pairs qualified |
| DOF | thin-lens extraction in `AetherCore.cpp` from CameraComponent | Shared thin-lens extraction; DofOn/DofOff pair qualified |
| Darkness volumes | `WorldRenderer::GatherDarknessVolumes` | Shared bounded extraction; Darkness/DarknessOff pair qualified |
| Object/blob shadows | `WorldRenderer::GatherBlobShadows` | Shared cooked-bounds/grouping extraction; Blob/BlobOff and Contact/ContactOff pairs qualified |
| Particles | `ParticleSystem` update/extract, `ParticleComponents.hpp` | Shared seeded portable simulation; fixed-seed 2D, 3D and display-only bursts qualified |

Particle world collision currently uses `Physics2DSystem`; full 3D collision and
gameplay physics remain Task 5. No particle GPU adapter may access the ECS.
`render2D.billboards` must contain copied data, and queued resources must retire
only after GPU work completes. Display-space particle flags differ from regular
UI sprite flags and must not be conflated.

## Portable custom passes

A project custom pass (`CustomPass.Register`/`Submit` in C#, `register_custom_pass`/`submit_custom_pass` in
Rust, gameplay-service ops 14-16) is one six-vertex fullscreen draw alpha-blended into the scene HDR colour at
`BehindScene`, `OverScene` or `EmissiveOverLight`. A shader is portable when it includes
`src/shaders/include/CustomPass.slangh` and reads only its accessors (view-projection, inverse
view-projection, elapsed time, camera position, viewport size, params, `color0`, `color1`, data count and
`CustomPassData(i)`). Natively the header compiles to the 80-byte push-constant contract; the Web cook defines
`AE_WEB_CUSTOM_PASS` and the same accessors become a 224-byte uniform record plus a read-only `vec4` storage
buffer (`WebCustomPassRenderer`). Native resolves the stem as `shaders://<stem>.spv`; the Web runtime loads the
cooked `project://assets/shaders/<stem>.wshader`. A missing or invalid shader fails registration with
`WEB_CUSTOM_PASS_SHADER`. Budgets enforced before GPU submission: 4096 float4 per submission (extra data is
dropped like C#), 64 submissions and 65536 float4 per frame, finite params/colors/data.

`BrowserCustomPass`/`BrowserCustomPassOff` (script `CustomPassProbe`, shader `custom_pass_fixture.slang`, 0.02
ambient, one lamp, one white sprite) differ only in the script's `active` flag and are judged by ordering
against the engine's own sprite and light pipeline rather than absolute colours: Behind is hidden by the
overlapping sprite, Over equals the lit sprite at the lamp-mirrored position, and the two Emissive rectangles
(on the lamp and outside its radius) render identically. Every rectangle carries a border on its min-x/min-y
edges so mirrored or flipped output fails. The `webgpu-2d-v1` publisher rejects
`.slang` assets (`WEB_SHADER_UNSUPPORTED`); `webgpu-3d-v1` cooks portable custom
passes to `.wshader` and excludes source `.slang` from the package. Unqualified:
elapsed-time/animated passes, 3D scenes, the non-HDR legacy path, projects other
than the reference fixture, and C# browser custom passes (the fixture script is
Rust; C# reaches the same `ExecuteCustomPassOperation` but no browser case drives it).

Negative Web fixtures `BrowserBadCustomPassShader` (unknown shader stem) and `BrowserBadCustomPassBudget` (17
full-size submissions, 69632 float4 against the 65536 frame budget) run as the `invalid-custom-pass-shader` and
`invalid-custom-pass-budget` browser cases and must fail with `WEB_CUSTOM_PASS_SHADER` / `WEB_CUSTOM_PASS_BUDGET`
diagnostics before any custom-pass GPU submission.

## Qualification rules

Each new case records backend/browser/adapter identities, authored fixture and
cooked shader hashes, camera and extents, exact simulation/capture tick, seeds,
effective graphics settings, diagnostic count, and queue retirement. Capture
both the enabled behavior and a neutral/disabled control; test that intentional
sign/order/strength regressions fail the semantic oracle. Keep whole-image error
tolerances unchanged unless a measured feature-specific reason is documented.

Include resize, odd extents, scene transition, missing resource, NaN/infinity,
over-budget and stop-in-flight cases. A build-only or screenshot-only result is
not visual qualification. Preserve the existing native Full/UiShell gauntlet,
C# and Rust production 2D exports, and private 3D language fixtures as separate
regression gates.

## Task 4 extraction implementation notes

`rendering/SceneEffectExtraction.hpp` now holds the existing native thin-lens,
world-sphere, actor-blob and darkness-volume packing math. `AetherCore` and
`WorldRenderer` use these helpers; Web scene extraction consumes the same DOF and
darkness helpers. This is a CPU sharing change, not evidence of effect rendering.
The portable helper tests cover lens response, transformed bounds, primitive
grouping, signed pickup radii, character budget priority and volume selection.

Web scene extraction snapshots the native graphics keys into
`RenderFramePacket.postEffects`. Project manifests with a graphics object inherit
compiled native graphics defaults before applying authored overrides. Internal
reference fixtures without that object retain neutral effect defaults. Invalid
number/boolean types and non-finite graphics values produce a field-specific
diagnostic. Fog/cloud/physical-sky, darkness, DOF and volumetric extraction is now
enabled because the corresponding GPU consumers are wired. This implementation
state does not qualify visual or temporal parity.

The maintained comparison helper preserves its original 21-case default scope.
Task 4's 22 additional scenes are opt-in: ten single-field effect/control pairs
(object lighting, baked lighting, gem, sheen, character, height fog, clouds,
physical sky, darkness and DOF), plus fixed-seed 2D and 3D particle bursts. Its
`--graphics-json <file>` option merges a typed finite JSON graphics object over
neutral settings and applies the identical result to the native private project
and browser manifest. Reports record that effective map. Same-image and black
outputs fail independent semantic controls; these cases remain unqualified until
the coordinated final effect suite passes. Browser required-case coverage adds the corresponding 12
effect checks and two pre-simulation particle invalid-input checks.

`ExposureMath.hpp` preserves native percentile-trim and logarithmic adaptation;
`WebExposureRenderer` meters the native 256x144 grid with footprint averaging and
retains exposure state through queued frames. Viewport-size changes replace state
with frame-owned references retaining queued resources. Exposure and ready
histograms persist across camera, scene and enable changes, matching native
policy. Native histogram metering runs every sixth render frame;
readback adapts exposure when the same one of three frame slots is reused. Web
retains the same histogram ring and runs adaptation before tonemapping, then
records the fresh histogram after tonemapping. This preserves native cadence
without CPU readback. Analytic HDR 2 temporal capture and frozen-frame evidence are qualified by the post summary; reset/toggle sweeps remain unqualified.

`post-parity-checks.py` is a separate, opt-in qualification harness for thirty
post controls. `BrowserPostPattern` contains mid-gray/warm/cool radiance, a tiny
HDR highlight and subpixel diagonal/stripe geometry; `BrowserPostCorner` supplies
real depth intersections and a reflective floor. Volumetric and camera-motion
fixtures reuse these controlled layouts. `BrowserPostExposure` fills the meter
with exact HDR 2: its histogram is bin140, allowing adaptation to be calculated
independently at each accepted frame rather than judged by convergence alone.
The harness preserves the lighting helper's existing image/probe thresholds,
requires enabled/neutral differences on each backend, and records both exact
graphics maps. It does not enable effects by scene name in production manifests.

Display-space particles have separate FXAA, sharpening and render-scale controls
to qualify their native position before the final resolve. `--extent 513x511`
requests odd output dimensions for GTAO/volumetric half-resolution resources;
the lighting helper accepts the same extent for the maintained DOF pair. Actual
native dimensions and effective render scale must match the request. A local
settings override produces an explicit unqualified diagnostic rather than a
change to user preferences or an even-resolution substitute.

The post harness explicitly requests a validated desktop-only per-run
`launchConfig.renderScale` override through `SettingsService.ApplyField` while
settings persistence is disabled. Both receipt streams must prove the actual GPU
source dimensions: native identity scale preserves the output extent; other
scales floor each axis and clamp to64..8192. The default lighting helper retains
its local-settings guard unless this isolation option is explicitly selected.
Eleven tonemap profiles use `BrowserPostTonemap`'s eight neutral radiance bands
and independent analytic display responses, including the gamma cancellation
for Hejl/Dice/AgX and the intentional Linear/Vanilla display alias. A fallback to
ACES fails every other operator's analytic probes.

Automation manual mode freezes temporal rendering between requested ticks.
Warm-up and resize redraws do not advance history. Every accepted manual tick
queues its own immutable readback, and `capture()` waits for the matching frame,
simulation tick, scene generation and dimensions. A declined submission retries
the same simulated state before accepting another tick. Motion qualification
therefore observes the moving frame instead of an extra stationary capture frame.
Native screenshot sidecars and Web GPU readbacks expose actual frame/exposure
evidence. The bounded post summary below qualifies these mechanisms for its recorded cases; it does not replace the remaining effect/material/native/language gates.

## Source-isolated prerequisite checkpoints

Local master source checkpoints: `c85ec677` (indexed scripts and attach-only lifecycle), `e0b18581` (curve UI and packaging), `1fd75cd3` (sampling, tiled lights and native shadow selection), and `7aced827` (accepted-frame temporal history). These commits exclude the concurrent streaming implementation; shared working files retain its unstaged hunks.

The prerequisite verification reports are `logs/parity-prerequisites/browser/report.json` (72 browser cases passed) and `logs/parity-prerequisites/native-unit.xml` (full native suite, zero failures/errors). `1f135bc3` updates the required browser audio case; `e7075422` makes the authority regression explicitly test fixed-delay interpolation without extrapolation. The reported 107.517 overshoot is compatible with production extrapolation, but its exact timing cannot be reconstructed from the assertion alone.

A separate committed-source export plus isolated rendering/history candidates builds without streaming overlays. `logs/render-themes/isolated-ui-smoke.json` records seven hardware-Chrome scene smokes covering UI, sampling, both tiled-light projections, seventeen local shadow maps, directional far fade and the exposure fixture; all start, render and stop without diagnostics. These are source-closure/runtime smokes, not additional native-image comparisons or proof of universal 1:1 parity. The earlier UI-only export and its receipts remain unchanged under `logs/parity-prerequisites/`.
