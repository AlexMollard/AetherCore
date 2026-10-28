# Fox sunset world: implementation and live execution plan

Date: 2026-10-27
Status: PLAYABLE REVISION IN PROGRESS — current user contract and execution state are in section15.
Project: `projects/FoxShowcase`
Tracking: session todo list plus the checkboxes below; no separate bead dependency.

## 1. User contract — retain every item after compaction

The user is Pog Champ. Requests accumulated in this order; later refinements override earlier visual choices, not the feature coverage:

- Create a browser project using the default animated fox from TestingProject.
- User explicitly chose the FULL supported 3D-effects showcase, not a simple scene reproduction.
- Support actual mouse and keyboard input; labels alone are not proof.
- Space the content out; the first tightly packed display platform was rejected.
- Make a visually interesting, cohesive, moody environment rather than a laboratory grid.
- Additional models and FOSS/open-licensed online assets are explicitly authorized.
- SUNSET: warm amber lighting, not bright midday; this supersedes the proposed cold moonlit scene.
- TRUE FREE CAMERA: independent position/look, not an orbit locked onto the fox.
- Proper ground materials and terrain; a stretched cube is explicitly rejected.
- A full laid-out map: wildlife, trees, rocks, mountains, beach, paths and connected landscape.
- The typed word “rockets” was interpreted aloud as rocks in the landscape list; user has not corrected that interpretation.
- Showcase the implemented browser-runtime engine capabilities throughout the world, not only its appearance.
- Make the project visible and playable from `http://127.0.0.1:18989/`.
- Persist the complete plan and resumption details so compaction does not lose the work.

Do not ask these decisions again. Do not silently reduce the map to a demo platform.
Do not claim unsupported browser networking, native editor panels or arbitrary shaders are working features.
Do not commit or push unless the user asks in the current message; preserve unrelated `.codex/` and `.omp/` files.
Keep the existing development gallery hosted on port 18989 throughout work.

## 2. Source of truth and existing work

### Project and previous publication

- Project was created through `scripts/Run-GameplayPipeline.ps1`, Rust, blank_3d.
- `projects/FoxShowcase/ProjectSettings.toml`: startup `FoxShowcase`, profile `webgpu-3d-v1`.
- `projects/FoxShowcase/scenes/FoxShowcase.scene.toml`: root scene includes the completed landscape, wildlife and interactions.
- `projects/FoxShowcase/scripts/src/lib.rs`: `ShowcaseCamera`, now true freefly; no language switch is needed.
- Source Fox: `projects/TestingProject/assets/models/Fox/Fox.gltf`, Fox.bin, Texture.png.
- Fox animation clips in order: Survey=0, Walk=1, Run=2.
- The hero Fox currently loops Run clip 2 at speed 0.8, scale 0.035, origin near `[0,0.04,0]`.
- Generated unused `default.scene.toml` and generated Player script were replaced, not kept as compatibility aliases.
- Published output: `projects/FoxShowcase/Builds/Web/FoxShowcase`.
- Distributable: `projects/FoxShowcase/Builds/Web/FoxShowcase/FoxShowcase.zip`.
- The published package is the final full map, including the corrected beach, mountain scree and grounded rocks.
- The original 23-entity dense platform was published, visually reviewed and rejected as too cramped/too basic.
- Do not mistake an old successful publish report for final-map verification.

### Shared engine changes already implemented

Rust previously had only key state and position setters, not mouse or rotation services.
Switching to C# would not avoid the gap: BrowserManaged.cpp also lacked its mouse/Euler exports.
The shared service extension is therefore implemented for real native and browser Rust gameplay:

| Operation | Public service | Safe Rust method |
|---|---|---|
| 31 | MOUSE_DOWN | Context::mouse_down |
| 32 | MOUSE_DELTA | Context::mouse_delta |
| 33 | SCROLL_DELTA | Context::scroll_delta |
| 34 | SET_EULER | Context::set_euler |

Files changed:
- `src/engine/scripting/GameplayServices.h`: operations 31–34, unchanged table layout/major ABI.
- `src/engine/scripting/GameplayRotation.hpp`: subtree-aware world Euler edit and physics synchronization.
- `src/app/scripting/RustGameplayServices.cpp`: native capability bits, pointer dispatch, checked Euler dispatch.
- `src/app/web/WebGameplay.cpp/.hpp`: focused input accumulation, fixed-tick consumption, pointer/Euler dispatch.
- `src/app/web/WebApplication.cpp`: scene-gameplay capabilities through op34; foundation path remains key-only.
- `resources/web/platform.js`: pointer movement/wheel accumulation and blur/visibility reset.
- `rust/aethercore/src/lib.rs`: safe wrappers; raw ABI records remain generated from existing headers.
- `rust/aethercore-abi/src/services.rs`: mirrored operation constants 31–34; service table layout is unchanged.
- `tests/scripting/RustGameplayServicesTests.cpp`: “Rust pointer and Euler services negotiate and validate inputs”.
- `docs/architecture/gameplay-abi.md`: documents the new services and capability boundary.

Finite values, live handles/world tokens and component presence are validated at service boundaries.
No page-only camera injection or fake mouse fallback is acceptable.

## 3. Ownership and live agents

Main agent owns integration, gallery routing, wildlife acquisition/population, interaction demos, publishing and final browser proof.
Do not duplicate a live agent's work; message its existing name first.

- `FoxLayout`: completed terrain/environment assets and SanctuaryLandscape; main retained final root scene/settings ownership.
- `FoxInput`: completed the camera and shared input work; main integrated module exports and final look-direction corrections.
- `FoxScene` and `ShowcaseControls`: earlier source authors, now idle; their initial dense-layout/orbit assumptions are superseded.
- Main owns `assets/credits.txt`, `assets/licenses/`, acquired wildlife assets, `Wildlife.scene.toml`, `Interactions.scene.toml` and new wildlife/interaction Rust modules.
- FoxLayout must INCLUDE `Wildlife` and `Interactions` by scene name, but must not edit those files.
- Before changing camera module layout, coordinate with FoxInput so it does not overwrite new module exports.
- Every delegated task skips builds/tests/formatters mid-flight; main builds after integration.

## 4. World layout and terrain contracts

Use a continuous map approximately 160×160 world units, not isolated exhibit pads.
FoxLayout supplied this working height/zone contract:

| Zone | Bounds / height | Purpose |
|---|---|---|
| Central meadow | `abs(x)<=20`, `abs(z)<=20`, flat y=0 | Hero fox, safe wildlife routes, entry and interactions |
| Beach slope | x=-64..-45; dry sand approximately x=-56..-45 | Gradual wet shoreline, dry sand and coast path |
| Ocean | Exposed offshore of approximately x=-56, surface y=-0.1 | Water/reflections/depth tint |
| Mountains / foothills | z<-35 | Strong skyline, rock strata, fog and trails |
| Eastern forest | x>25 | Grouped trees, foliage wind, godrays and forest paths |

The exact height sampler is:
`projects/FoxShowcase/assets/models/sanctuary/terrain_height.py`.
Keep initial wildlife routes in the flat central meadow unless using the actual sampled terrain heights.
Never let animals skate through hills, float over paths or swim accidentally because y was assumed constant outside the meadow.

Composition requirements:
- Open hero clearing and readable fox silhouette; paths connect the zones.
- Natural tree/rock clusters and asymmetric ruins frame views, not a symmetric ring of test cards.
- Proper tiled grass/soil/stone/sand materials, normals and roughness; meaningful UVs and shoreline geometry.
- Mountains are terrain/landforms, not stretched cubes.
- Sunset is low and amber with long shadows, muted terracotta horizon and dusk-blue upper sky.
- Use restrained exposure/emissive strength; preserve readable darks without washing the fox white.
- Integrate effects as landscape landmarks: thermal/volcanic rocks, crystal grotto/relics, forest shafts, fireflies, coastal water.
- Replace rainbow test bars/green ramp cards with cohesive materials while keeping the demonstrated vertex-colour/UV-scroll/foliage features.
- Real soft-alpha particle sprites, not opaque coloured squares.

## 5. Capability coverage matrix

Every row needs an actual authored/runtime demonstration; setting a flag alone is not proof.

| Capability | Planned visible/interactive use |
|---|---|
| Skinned animation | Hero fox plus animated wildlife, distinct live poses |
| Mouse + keyboard | True freefly exploration, focus/blur safety, speed control |
| Meshes / imports / transforms | Multi-primitive nature props, ruin architecture and landscape |
| PBR textures | Tiled terrain albedo, normal, metallic/roughness and AO where applicable |
| Lighting/shadows | Low sun plus bounded local shadowed warm lights; long terrain/prop shadows |
| Sky mesh / physical sky / clouds | Warm low sunset disk, sky backdrop, moving clouds |
| Water + SSR | Coast/ocean/pond with real foreground depth and reflections |
| GTAO / contact / blob / darkness | Ruin corners, grounded actors/props and shaded grotto |
| Specialized materials | Gem, sheen, character, object-lit, baked, vertex-colour, wind foliage, UV scroll, light-shaft |
| Generated shaders | A material-graph relic, not a fake coloured substitute |
| Built-in animated effects | Molten/plasma integrated into thematic landmarks |
| Particles | Warm fireflies/embers, optional coastal particles; bounded continuous emission |
| Post stack | Volumetrics, DOF, bloom, auto exposure, motion blur on camera travel, grade/lens/grain, FXAA/sharpen/render scale |
| 3D physics / joints / queries | Interactive bounded physics objects / query-driven interactions near the clearing |
| Prefabs / includes | Wildlife and interaction sub-scenes; a real spawned prefab if included in interactions |
| Audio / durable saves / UI | Small real interaction sound and saved interaction/visit state, with concise guide/status feedback |

No unsupported networking/editor/custom arbitrary Slang claim. Do not loosen image/gameplay tolerances to make a new scene pass.
Existing parity qualification does NOT automatically qualify every new artistic combination.

## 6. Assets and licensing — preserve these facts

Already acquired and copied to `assets/models/sanctuary/`:
- tree.glb: Nature `tree_detailed_dark`, primitives 0/1/2.
- rock.glb: `rock_largeA`, primitives 0/1.
- column.glb: `statue_columnDamaged`, primitives 0/1.
- bridge.glb: `bridge_stone`, primitives 0/1.
- moss.glb: `hanging_moss`, primitive 0.
- mushroom.glb: `mushroom_redGroup`, primitives 0/1.
- arch.glb: Castle `wall-doorway`, primitive 0, external `Textures/colormap.png`.

Every primitive needs an entity/material or an equivalent complete model expansion; do not silently omit trunks/leaves/parts.
Nature dimensions before scale: tree~0.85×1.33×0.76, rock~0.79×0.26×1.02, column~0.3×0.7, bridge~1.04×0.4×1.04.
Raw Nature materials contain metallicFactor=1; authored scene overrides should use appropriate nonmetal materials.

Primary source URLs:
- https://kenney.nl/assets/nature-kit
- https://kenney.nl/assets/castle-kit
- Nature ZIP: https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip
- Castle ZIP: https://kenney.nl/media/pages/assets/castle-kit/a395102d20-1711543616/kenney_castle-kit.zip
- Cached originals: `.temp/fox-nature.zip`, `.temp/fox-castle.zip`.
- Licenses are CC0; the Nature archive's license labels itself 2.1 even though the page update list said 1.0.
- `assets/licenses/kenney-nature.txt`, `kenney-castle.txt` preserve upstream notices.
- `assets/credits.txt` preserves original Fox CC-BY4 attribution and Kenney attribution/source links.
- Publish additional_assets includes credits, all three license notices and the arch colormap.

IMPORTANT resolved importer issue:
The original Nature UniGLTF GLBs referenced node1 as scene root while identity node0 also parented node1.
cgltf correctly rejects scene roots with parents (dependency cgltf.h around6784).
Main normalized SIX project GLBs' scene roots to their ancestor node0, preserving geometry/BIN/materials/transforms.
Do NOT overwrite repaired project GLBs with the raw ZIP originals.
`AssetPacker bake-all projects/FoxShowcase` then successfully baked all 11 existing model assets.

Wildlife acquisition and integration are complete:
- Official CC0 pack: https://quaternius.com/packs/ultimateanimatedanimals.html
- Mirror/bundle: https://poly.pizza/bundle/Animated-Animal-Pack-ILAPXeUYiS
- Two deer and two wolves use real embedded-buffer glTF assets.
- Deer Walk=12/Eat=3; Wolf Walk=11/Idle=5. One individual of each species follows a bounded meadow route.
- `assets/licenses/quaternius-animals.txt` preserves the pack-specific CC0 source receipt.

## 7. Free-camera implementation contract

Script type remains `ShowcaseCamera` so scene bindings stay valid.
Most recent FoxInput report:
- Left-drag LOOKS freely; arrow keys are keyboard look.
- WASD flies; Q/E descends/ascends.
- Wheel changes travel speed, default10 units/s, range1–100.
- Shift multiplies speed by3.
- R resets position/orientation/speed.
- Exposed properties: position, yaw, pitch, move_speed.
- Initial camera `[10,8,17]`, initial Euler approximately `[-19.54,30.4655,0]`.
- No automatic sway, orbit-target lock or page-only transform shim.
- Camera far plane is300 and accommodates the full map.

## 8. Gallery integration — completed user-visible access

FoxShowcase is now a separate Playable project card on18989.
Implementation:
- `tools/web-runtime/serve-preview.py`: curated project cards and package-root-contained routing.
- `resources/web/gallery-catalog.json`: separate projects.FoxShowcase metadata.
- `tools/web-runtime/test_serve_preview.py`: catalogue validation and real HTTP package/traversal checks.

Implemented curated project entry separate from reference `scenes`:
- Add a projects entry for FoxShowcase with metadata and its published directory.
- Use a same-origin link such as `/projects/FoxShowcase/`, not a dependency on the separate18990 service.
- Gallery renders project cards alongside existing reference examples, marked Playable, NOT Matches native.
- Preview server routes that declared project's files from its published directory, preserving nested relative URLs, Wasm MIME and no-store.
- Resolve paths and reject traversal outside the explicitly declared package root.
- Do not expose arbitrary repository files or add a broad raw-directory server.
- Add meaningful routing tests for package access and traversal denial; preserve existing catalogue invariants.
- Restart ONLY gallery18989 after server source edits; immediately restore persist mode and verify HTTP/browser link.
- The package can be replaced atomically on republish; serve from repo CWD rather than holding its final directory open.

## 9. Execution phases and acceptance

### Phase A — durable plan and immediate discoverability
- [x] Persist this full user contract, ownership, evidence and exact next actions.
- [x] Add curated project card and safe same-origin package routing to main gallery.
- [x] Exercise card in browser and confirm the package opens from18989.

### Phase B — complete landscape/environment
- [x] Accept FoxLayout's full map/terrain/art output, review actual authored files and encoding.
- [x] Confirm central flat meadow, forest, foothills/mountains, paths, beach/coast/ocean and all visual capability landmarks.
- [x] Ensure both included sub-scenes exist before invoking publisher.
- [x] Parse with native gameplay inspection; source text must be proper CRLF, never CRCRLF.

### Phase C — wildlife and capability interactions
- [x] Acquire selected real animated CC0 wildlife models and their complete dependencies.
- [x] Bake them, inventory mesh primitives, clip names and dimensions from real model data.
- [x] Author `Wildlife.scene.toml` with several species/individuals and bounded deterministic movement on valid terrain.
- [x] Add wildlife Rust module and exports without overwriting ShowcaseCamera.
- [x] Author `Interactions.scene.toml` and bounded meaningful physics/query/prefab/audio/save/UI demonstrations.
- [x] Keep guide concise and controls non-conflicting with freefly navigation.
- [x] Register dynamic scenes/assets explicitly in publish.web declarations.

### Phase D — build, publish, interact and review
- [x] Build current scripts and publish the ACTUAL final map, not an earlier sanctuary.
- [x] Hardware browser: start via real gesture; verify mouse look, keyboard movement, speed change, reset and blur safety.
- [x] Observe different animation poses while camera is stationary; inspect wildlife movement/ground contact.
- [x] Fly through meadow, forest, mountain trail and beach; screenshot each zone and sunset overview.
- [x] Exercise real gameplay demonstrations and save/reload/audio behavior; inspect diagnostics and browser errors.
- [x] Retune art from screenshots until warm/subdued/readable, without dropping effects or map scope.
- [x] Verify relocated ZIP/iframe/bridge-free boundary and final gallery link.
- [x] Update existing docs/web-export.md and this plan with final evidence and remaining honest limits.

## 10. Commands, tools and evidence

Always pin native project tool in the SAME invocation:
`AETHER_PROJECT_TOOL=D:/AetherCore/build/default/src/app/RelWithDebInfo/aether-project.exe`.
Playwright module:
`C:/Users/alex.mollard/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright`.
Slang: `C:/VulkanSDK/1.4.350.0/Bin/slangc.exe`.
Emsdk: `.temp/toolchains/emsdk-main`.

Publish request already exists: `logs/fox-showcase/publish-request.json`.
Run `python tools/project/web_publish.py --request <request> --report <fresh-report> --directory <fresh-private-directory>` with Slang/Playwright environment set.
Native inspect: `scripts/Run-GameplayPipeline.ps1 -Operation inspect -Project projects/FoxShowcase -Scene FoxShowcase -ReportPath <report>`.
Native gauntlet: `pwsh -NoProfile -File scripts/Run-DebugGauntlet.ps1 -ReportPath <report>`.
Browser regressions: `scripts/Run-WebRuntimeChecks.ps1 -SkipHostBuild -BrowserChecks` with explicit AssetPacker/Slang/Playwright paths.
After new C++ headers/source changes, run `cmake --preset clangd`; after code changes, run `graphify update .`.

Already exercised on shared input-services tree, BEFORE final map:
- `logs/fox-showcase/native-gauntlet.json`:1563/1563 native cases, zero editor/runtime Vulkan findings.
- `logs/fox-showcase/web-runtime-report.json`:72/72 hardware browser cases.
- `cargo test --manifest-path rust/aethercore/Cargo.toml`:21 passed.
- `logs/fox-showcase/publish-sunset-final.json`:earlier sanctuary publication passed, not final-map proof.
- `.temp/fox-showcase-sunset-final/web-verification.json`:earlier relocated package evidence.
- Source GLB bake:all11 original model assets baked after upstream root repair.
- The final map and freefly revisions were republished and exercised; final evidence is listed in section13.

Live services at checkpoint:
- `gallery-projects-18989`:persistent main gallery, pid20024, serving the final package.
- `fox-showcase-final-18990`:persistent Python server pid56504, repo CWD, serves published FoxShowcase via --directory.
- No browser tab is intentionally retained; earlier fox-showcase tab was closed.

## 11. Known traps and rejected shortcuts

- Do not repeat a full repo onboarding; start at the next unchecked phase.
- `find` is broken because its judge provider returns402; it is NOT evidence that files are missing. Use scoped grep/read/glob fallback.
- No mounted codebase-memory graph tools were available in the visible inventory; use available LSP for symbol references.
- Source scene once contained CRCRLF; native TOML parser rejected it although Python universal-newline read appeared valid. Use byte checks and proper write normalization.
- Browser tabs can become hidden after reload; use page.bringToFront() and real visible/focused canvas before input checks.
- Bootstrap status running/state ready is NOT gameplay ready; wait assetsReady, click Play, wait rendererReady/ticks.
- Export has no test bridge; do not claim production tick-exact state capture. Observe actual canvas/input or build a separately identified instrumented sibling.
- Python eval does not have Pillow installed; no dependency was added. Use browser-native image decoding or normal screenshots for visual proof.
- The first DOF aperture0.12 blurred most of the scene; aperture4 retained DOF while improving readability.
- Fake full feature support, hidden diagnostics, shader tolerance changes and replacing terrain with another cube are not acceptable.
- Gallery card metadata must not mark this artistic combined map as native-qualified without actual parity evidence.
- Keep original TestingProject, shared scene templates and unrelated gallery status edits intact.

## 12. Completion and future maintenance

No implementation or verification phase remains open.
Start future changes from the current source and section13 evidence, not an older sanctuary snapshot.
Keep the18989 gallery service running; published project files are served from the declared package root.
If source changes, publish to a fresh private directory/report and repeat real browser input, zone and interaction checks.
The card remains Playable: this combined artistic map has not received separate native-versus-browser parity qualification.
No commit or push was requested or performed for this work.

## 13. Latest execution checkpoint — supersedes earlier live-state notes

The full world, wildlife and interactions are now integrated and published.
The earlier “acquisition not done” and “old sanctuary only” notes above are
historical context, not the current implementation state.

Completed source integration:
- `lib.rs` imports `interactions` and `wildlife` and exports ShowcaseCamera,
  WildlifeWalker and RuinExperiment. Main owns this file; FoxInput must not
  overwrite it with a standalone camera draft.
- Freefly is real and independently positions/rotates the camera. Arrow-key
  signs were corrected: Right looks right and Up looks up.
- Wildlife includes two deer and two wolves; each model's primitives share
  correct transforms/skin animation. Deer Walk=12/Eat=3, Wolf Walk=11/Idle=5.
- Interactions include a static meadow collider, a dynamic resting stone,
  hinged relic, bounded launched prefab, actual raycast, original WAV chime,
  durable progress save and a concise two-line HUD.
- G reset synchronizes the stone's physics position by calling set_euler after
  set_position, before resetting velocity.
- The HUD now has one root canvas and separate child text rectangles; its
  dark backing fixes the earlier overlapping/unreadable text.
- Explicit publish assets include RuinStone prefab, ruin-chime.wav and
  Quaternius license in addition to the earlier model notices.
- Main root scene now explicitly enables physics_3d.
- Final authored scene counts: root67, SanctuaryLandscape171, Wildlife26,
  Interactions10 (HUD backing included).
- Northern terrain now has roughly 28–32-unit peaks; additional woodland
  clusters brought landscape trees to46, plus root-tree props.
- Main re-grounded geothermal root landmarks for the raised mountains:
  Molten y10.754, Plasma y12.832, graph y1.667,
  west shrine light y12.504, shrine bases y9.684 and11.762.
- Latest sunset tuning: sun direction[.78,.12,.56], colour[1,.56,.30],
  intensity1.3, auto-exposure key.085.

Gallery integration is implemented and exercised:
- Catalogue has a separate `projects.FoxShowcase` Playable card.
- `serve-preview.py::project_asset` resolves only registered package roots,
  checks resolved containment and returns404 for escapes/unknown assets.
- Same-origin URL: `http://127.0.0.1:18989/projects/FoxShowcase/`.
- Two gallery tests pass, including real HTTP nested assets/Wasm MIME and
  encoded traversal denial.
- Main gallery was restarted as `gallery-projects-18989`, pid20024, persistent.
  The old `gallery-18989` process is stopped.
- Separate18990 service remains persistent but is no longer required to launch
  the project from the main gallery.

Real final-world browser verification already passed:
- `logs/fox-showcase/publish-qualified.json` and
  `.temp/fox-showcase-world-qualified/web-verification.json` record successful
  final relocated ZIP/iframe/bridge-free production checks.
- `logs/fox-showcase/world-smoke.json` records a real Playwright hardware-Chrome
  click from gallery to package, mouse look, keyboard flight/reset, wheel speed,
  strafe, F/G interaction, actual scheduled audio and committed save,
  reload restoration, and a keyboard-driven forest/beach/mountain/overview/
  wildlife tour. All diagnostics/page errors were empty.
- Actual interaction counters were committedWrites1 and scheduledSounds1;
  reload read `fox_ruin_experiment == "completed"`.
- `world-wildlife-a.png` and `world-wildlife-b.png` visibly show distinct
  walking/grazing deer poses with the camera stationary.
- `world-forest.png`, `world-mountains.png`, `world-beach.png`,
  `world-overview.png` and interaction screenshots are under logs/fox-showcase.
- The throwaway Node smoke driver was removed after final proof.
  JSON/screenshots remain local verification evidence.
- Browser Eval/managed Chrome click helpers repeatedly failed while the same
  link worked through actual Node Playwright. A tool issue was reported.
  A Bun Playwright attempt timed out/reset the JS VM. Do not repeat it:
  run the Node driver for reliable input proof. Reacquire managed tab handles
  by name if needed; JS variables from that reset are gone.

Final visual correction pass is complete and reviewed:
- Broad landscape rock pairs use minimum rotated-footprint terrain samples
  with slight embedding; final beach/mountain captures show grounded bases.
- High and steep northern cells use a separate scree mesh with stone albedo
  and normal textures instead of uniform grass.
- The reprofiled beach has an approximately11-unit dry sand band and a
  gradual wet shoreline; this is visible in the final coast capture.
- All four terrain meshes were rebaked by the final publication pipeline.
- Final hardware-Chrome smoke passed with no page errors or diagnostics:
  mouse changed visible geometry fraction0.6212, keyboard fraction0.5751.
- Final host regressions:61 project cases (6 skipped) and92 web-runtime
  cases passed. Shared native1563/browser72/Rust21 checks remain recorded above.
- `.gitignore` now excludes per-project Rust scripts/target build output.
- Final graphify update completed:1700 code files,32921 nodes,52741 edges.

## 14. Coherent living landscape revision — 2026-10-28

User rejected the prior result: animals appear to walk in place, ground does
not read as grass, content is still condensed in the centre. This revision
must fix actual world movement, grass material plus blade geometry, and
habitat/destination layout, not merely add more props to the same clearing.

Agreed implementation from source inspection:
- Remove the static running Fox from the root scene. Wildlife now owns Fox.
- Broad terrain-following routes: Fox centre(-12,24), radii(10,6), speed1.8;
  deer centre(-30,16), radii(8,12), speed1.35; wolf centre(42,5),
  radii(10,14), speed1.55. Ten seconds travel, three seconds rest.
- Resting deer(-27,30) eats; resting wolf(51,-10) idles.
- Fox rests with Survey0, travels with Walk1; never Run2 while stationary.
- New GroundCover include:24 spatial batches, approximately24K grass tufts,
  rooted foliage wind, varied blade heights/colours, no grass on paths/water.
- Grass terrain texture repeats every4world units; use white material tint
  and high roughness, not the old dark beige modulation.
- Narrow dirt trails connect beach(-47,25), pond shore(-30,36),
  ruins(-5,-22), forest grotto(46,-25), eastern woodland and geothermal
  foothill sites. No broad tiled crossroad or stripe up the mountain face.
- Main owns root scene, landscape redistribution, pond basin/surface,
  interaction relocation, camera opening, publication, docs and browser proof.
- HabitatWildlife completed wildlife.rs/Wildlife scene. Main corrected its
  erroneous Fox rest clip2 to Survey0. LivingGrass completed grass/trails.
- Terrain unchanged except a shallow pond basin within the grass exclusion;
  all wildlife routes remain outside it. Re-ground relocated props.

Pending acceptance:
- Integrate GroundCover and remove old leaf-ramp exhibit.
- Spread landmarks into connected sites; keep animal corridors unobstructed.
- Run project Rust tests and fresh production publish.
- Observe real translational movement against stationary landmarks, including
  rest/clip transitions; pose changes alone do not prove locomotion.
- Review near-ground grass, animal habitats, pond, ruins, coast and overview.
- Record new evidence here; old section13 evidence is not revision proof.

## 15. Current playable-scene contract and execution — supersedes sections13/14

Latest user requirements, all mandatory:
- Preserve low-poly art. The user explicitly clarified that the low-resolution
  complaint concerns the swapchain/framebuffer, NOT polygon count.
- Replace freecam with a third-person Mixamo character, existing idle/walk
  animation, real movement and collision. Turn the world into a playable scene.
- Nearby objects open an interactive 2D capability UI; fix clipped glyph bottoms.
- Camera-local grass detail, proper frustum rejection and smooth distance fade
  into the distant grass material, without a hard visible coverage boundary.
- Correct excessive lighting/bloom/auto-exposure across affected scene rendering.
- Keep all earlier landscape/wildlife/material/world requirements and18989 access.

Current integration, not yet accepted as final:
- `player.rs` and `Player.scene.toml`: Explorer capsule, full Sandbox Mixamo
  model+buffer.bin/walk.bin, scale.01, idle0/walk1, movement measured for clips.
  WASD walk2.1, Shift brisk3.4, Space jump, left-drag orbit, wheel zoom,
  R respawn. `lib.rs` exports ExplorerController/WildlifeWalker/RuinExperiment;
  old ShowcaseCamera is removed. Root now includes Player and TerrainCollision.
- Player and interaction module share `player::CONTROLS_BLOCKED` AtomicBool.
  Modal journal blocks movement/orbit; attach/close clears the flag.
- `interactions.rs` now uses proximity E, Escape close, discovery/save mask
  `fox_world_sites`, five journal sites. At ruins, F launches physics stone,
  G resets. No global remote F interaction outside the ruin journal.
- Five sites: trailhead(1,30), pond(-30,37), ruins(-5,-16),
  forest shrine(46,-25), coastal lookout(-47,25).
- `FieldJournal.prefab.toml`: real canvas/text, generated field-map UIImage,
  native interactive slider/toggle. No fake settings or inert buttons.
  HUD now uses960x540 scale_with_reference, proper row heights/new controls.
- All world static collision is in TerrainCollision:4 exact indexed terrain
  quadrants,1681vertices each (below4096 collider limit); tree trunks/rocks/
  columns plus scale-baked exact doorway/bridge mesh collision,139 bodies.
  Terrain follows actual height including pond basin. Native mesh_source is
  supported; do not replace collision with position-clamping fake grounding.
- Terrain defect found and repaired:419 cells were absent from the old
  wide-trail mask. New narrow trails are overlays above continuous ground.
  The tan mountain stripe was sky through these holes, not a lighting effect.
- Landscape now104 distributed trees in groves plus root trees, with entire
  wildlife habitats and narrow paths clear. Two-part trailhead marker added.
- Grass25,476tufts/76,428triangles in24 spatial patches. A temporary denser
  curved-blade version was reverted after the user's framebuffer clarification.
- GroundCover is MAIN-owned: vertexcolour+foliage, baked_lighting=false,
  receive_shadows=true, cast_shadows=false, distance_fade=[28,45].
- Root ambient[.28,.29,.23], sun[1,.66,.38] intensity1, warm sunset retained.
  Project bloom.035, exposureKey.06, exposureSpeed.8, exposure.85,
  GTAOstrength.65, filmGrain.015. Auto-exposure stays enabled.

Shared renderer changes completed by agents, parent review/build in progress:
- scene material `distance_fade=[near,far]`, default[0,0] disabled;
  standalone .material key `distancefade`. GPU material80→96bytes.
- FrameConstants2576→2592bytes adds explicit main-view visibility camera;
  native shadow/cull/color/prepass and WebGPU share ordered dither coverage.
- WebGPU static bounds cull off-frustum/far meshes before draws and bindings.
  Skinned/procedural/custom/sky bounds remain conservatively visible.
  Counters: aetherRuntime.meshCameraVisible/meshFrustumCulled/meshDistanceCulled.
  Instance buffer still retains original packet indices; no compaction claim.
- Exposure state now resets on scene generation or auto-exposure enable change.
- Browser font atlas uses measured Latin-1 ascent/descent; label alignment uses
  measured glyph block height. Parent cached font metrics on atlas upload.
- DPR defect PROVEN on old published package: canvas1038x584 for CSS1038x584
  at devicePixelRatio1.25; desired1298x730. WebApplication.cpp resize previously
  ignored DPR. Parent now multiplies CSS dimensions by DPR, uniformly capped
  to existing4096 surface limit. Test-bridge sizing remains explicit.
  Project renderScale now1.0 (previously.9). Verify final physical dimensions.

Verification jobs launched:
- bg_69 native default Editor/GameRuntime/EngineTests/AssetPacker/aether-project/
  WebMaterialCompiler build completed,172.53s, output artifact://713
  (raw artifact://712). Shared native shaders compiled successfully.
- bg_71 current project cargo test:1 passed. Earlier bg_66 same.
- bg_72 rebuilding EngineTests after deleting a new copy-only packing test,
  then CTest. Await delivered verdict; never poll.
- bg_70 fresh production publication:
  logs/fox-showcase/publish-playable-review.json,
  .temp/fox-showcase-playable-review. Await delivered verdict.
- bg_73 full browser runtime regressions:
  logs/fox-showcase/living-regressions/report.json. Await delivered verdict.
- Old candidate publish-living-world.json passed but showed terrain holes,
  old freefly and unlit grass; it is NOT final playable-scene evidence.

Exact remaining work:
1. Read job deliveries, fix any real failure. Finish renderer raw-hunk review.
2. Inspect new package through18989, prove backing dimensions at DPR1.25/2,
   actual walking/jump/collision/idle, camera obstruction and R respawn.
3. Open trailhead E journal, manipulate real slider/toggle, verify player is
   blocked, Escape closes all UI children, save survives reload.
4. Walk to remaining sites, exercise ruin F/G; capture the world at ground
   level, moving wildlife against fixed landmarks and grass culling counters.
5. Check restrained highlights, correct textured grass/no holes, glyph
   descenders and continuous near/far grass coverage. Fix actual findings.
6. Run native bounded gameplay assertions/validation smoke as needed for the
   shared layout/shader changes; keep parity tolerances unchanged.
7. Update docs/web-export.md and this plan with final evidence, mark todos
   only after real acceptance, remove throwaway scripts and close own browser.
8. Keep persistent gallery-projects-18989 service running. No commit/push.

Managed browser tab `fox-living-review` is open through Eval browser; it was
showing the old living-world candidate. Reload after publication. A Node
Playwright driver remains available as an alternative for trusted long tours;
no production test bridge or direct transform injection is acceptable proof.

## 16. Latest feedback and execution checkpoint

Additional user requirements (retain ALL):
- Controller feels stiff/slow: tune acceleration, braking, smooth body turns
  and camera following, not just raw speed.
- Asset scales must make sense against the1.81m humanoid. Fox was2.76m tall
  at.035 and was genuinely oversized; latest source scale.008 gives.632m.
- AFTER grass is fully verified, promote it into a reusable BUILT-IN ENGINE
  FEATURE and migrate FoxShowcase. This is a required remaining phase, not
  an optional future suggestion. Research is done; implementation not started.
- User physically walked into the sphere and gameplay stopped with
  BROWSER_SCRIPT/BROWSER_GAMEPLAY `physics event failed (gameplay status4)`.
  Treat this as the failing reproduction; do not rerun just to confirm it.

Current changes after section15:
- Player now3.7/5.6m/s, acceleration21, braking30, smooth shortest-angle
  body turning, measured-displacement animation speed and smooth camera.
  Camera recentres only after forward input stops, avoiding direction feedback.
  Parent caches animation speed changes and restores idle speed1.
- Explorer now implements Script::event for actual physics events, increments
  public collisions on CollisionEnter and refreshes grounded state.
  Nonphysics events still return UNSUPPORTED; no diagnostics suppressed.
  Error constant is `aethercore::abi::AE_GAMEPLAY_UNSUPPORTED`, NOT the private
  aethercore root import. First compile attempt failed and was corrected.
- Wolves.44→.36 (~.96m tall), Fox.035→.008 (~.63m), deer stays.38 (~1.62m).
  Ruin pillars6→4 (~2.8m); main arch3→2.2 (~2.88m); smaller gate now1.9
  vertical (~2.49m). Mushroom groups3→.6 (~.15m). Coastal columns reduced.
  Doorways now face90degrees toward the trail. Scale-baked collision matches.
- TerrainCollision now147 bodies, including resized props/trailhead/stepping
  stones and exact small-gate/main-gate/bridge mesh colliders.
- Found another UI authoring cause: real field name is `pixel_size`, not
  `size`. Corrected all Fox HUD/journal labels; otherwise they defaulted to24
  and overflowed their boxes. Browser text now word-wraps at spaces.
- Lit foliage wind was incorrectly gated on baked-lighting flag64. Shared
  FoliageWindMath now animates foliage independent of baked/dynamic lighting;
  sky exclusion remains. Existing prelit foliage behavior is unchanged.
- Runtime now exposes renderWidth/renderHeight, so swapchain/backing AND
  actual scene framebuffer dimensions can be verified, not merely CSS size.

Observed browser acceptance on previous playable-review package:
- Main18989 package launched third-person Explorer with real controller.
- Actual canvas1298x730 at CSS1038x584/DPR1.25: DPI fix proven.
- Culling159visible/293frustum-rejected/9distance-rejected at start.
- E near trailhead opened real journal with map/slider/toggle; audio scheduled1,
  committedWrites1. Escape destroyed journal children and runtime stayed healthy.
- Screenshot exposed oversized Fox and wrong font sizes; those are now fixed
  in SOURCE but were not yet visually accepted in the latest publication.
- Managed tab `fox-living-review` remains open. Most recent package may still be
  older until the pending publication succeeds; reload before any new proof.

Verification outcomes:
- Native build bg_69 passed, native shaders compiled; CTest bg_72 passed4/4.
- Browser regression bg_73 failed because sccache transport connection reset
  while compiling MoreComponents, NOT a source diagnostic.
- Retried with SCCACHE_DISABLE=1 as bg_74: PASSED72/72 cases; report
  logs/fox-showcase/living-regressions/report.json.
- Project cargo tests passed1 case.
- publish-playable-review.json passed; later source changes invalidate its
  claim as final revision evidence.
- Native regression config now projects/FoxShowcase/playable-test.json:
  run500ticks at step1/30, walk left66ticks then north406ticks, E ruin journal,
  F sphere/prefab experiment, G reset, Escape close; assert actual movement,
  grounded/idle state, journal open/teardown and2 real audio starts.
- bg_75 publish-player-collision and bg_76 native-playable failed on the private
  constant import. Preserved their reports. Fixed import before retry.
- CURRENT jobs: bg_77 publishes to .temp/fox-showcase-player-collision-checked,
  report logs/fox-showcase/publish-player-collision-checked.json.
- CURRENT bg_78 runs the500tick native scenario, report
  logs/fox-showcase/native-playable-checked.json. Await delivered results.

Built-in grass discovery (read before implementing that phase):
- Completed read-only agents GrassRuntimeSurvey and GrassAuthoringSurvey.
  Their full compressed source maps are in agent://GrassRuntimeSurvey and
  agent://GrassAuthoringSurvey; no code edits from them.
- Prefer a real reflected GrassComponent plus shared deterministic CPU
  MeshGen::MeshData generator and normal mesh chunk rendering. Reuse generic
  distance/frustum/shadow paths; do not create a custom grass render pass.
- Runtime must generate/sample on game/asset side; render thread never ECS.
  CPU terrain data can be read from assets::GltfAsset baked geometry (physics
  already does this), even though there is no first-class Terrain API.
- Native Mesh::Create/LoadedModel ownership and browser UploadMesh/WebModels
  are actual reusable upload paths. PrimitiveMeshes is a fixed five-mesh cache,
  not a keyed dynamic chunk owner; do not distort it into an unbounded registry.
- Need concrete safe chunk lifetime/retirement, terrain sampling/exclusions,
  reflection/scene+editor authoring, native/browser extraction and web package
  dependencies. A cooked .grass.toml type is not automatically simpler.
- User wants promotion AFTER grass is complete; finish current grass proof,
  then implement, migrate away from24 project-specific baked grass glTFs and
  verify both backends. Do not silently leave this phase unfinished.

## 17. Collision fix accepted; built-in grass implementation underway

- `publish-player-collision-checked.json` PASSED, committed production package
  is live at http://127.0.0.1:18989/projects/FoxShowcase/.
- Managed tab `livingTab` / fox-living-review reloaded and started this package.
  Observed canvas AND actual render framebuffer1298x730 at DPR1.25.
- Real keyboard A132ticks/W812ticks reached Sanctuary relic. Runtime stayed
  running through tick4548+, no diagnostics. E opened the correct ruin journal.
  Saved screenshot: logs/fox-showcase/sphere-contact-journal.png.
- F launched actual stone/prefab interaction, G reset; actual keyboard Tab/
  arrows/Space changed slider to.6 and toggletrue. Audio scheduled2, Escape
  removed all journal children (425→106glyphs), no gameplay stop.
- Font sizes/descenders and wrapping visually accepted in the saved screenshot.
  Correctly scaled fox visible beside normal humanoid in initial view.
  Full-resolution textured grass/terrain has no old path holes.
- Native initial500tick scenario failed at update because new models had not
  been baked in native project assets (invalid mesh magic; missing animation).
  AssetPacker bake-all projects/FoxShowcase baked42,11up-to-date.
- Retried actual native scenario: PASSED fixed ticks + state assertions;
  logs/fox-showcase/native-playable-baked.json, job66664c0e-eb1f-4310-b274-ed8e7fbca854.
  Final Explorer collisions=1, groundedtrue, movingfalse, position
  [-5.632925,.00000513,-15.561472]. Sphere contact is proven, not inferred.
  Native custom BrowserGraph shader still logs a missing pipeline; fix native
  graph artifact before claiming complete native visual showcase.

Built-in grass interface now fixed:
- engine scene/GrassComponent.hpp: enabled, surfaceMesh, exclusionMesh,
  regionMin/Max, spacing, chunkSize, heightRange, bladeWidth, seed, base/tipColor.
- engine mesh/GrassMeshData.hpp/.cpp: GenerateGrass, LoadGrassMeshes,
  GrassMeshKey. Shared jittered deterministic grid; terrain triangle height
  interpolation, projected exclusion triangles, chunk-local meshes and origins.
  Empty surface means explicit flat Y=0. 262144candidate/4096chunk cap.
  Blades use uv2.x anchored0/tip1 for existing shared wind.
- Parent owns generator, tests, CMake, Fox migration and verification.
- NativeBuiltinGrass completed shared Grass reflection, scene runtime prep/
  upload in ScriptedSceneLayer::OnUpdate, scene-context retained mesh vectors,
  and WorldRenderer normal/shadow chunk extraction. Files must be reviewed.
- WebBuiltinGrass completed browser extraction/cache and publishing Grass
  allowlist. Existing URI reachability discovers the two source model paths.
- CMake/WebSources.cmake now includes mesh/GrassMeshData.cpp.
- No builds since these edits yet. New generator initially had incorrect
  AE_UNEXPECTED double-return and AE_TRY result accesses; parent correcting
  these using actual Expected.hpp macros + LSP quick fixes.
- IMPORTANT fix pending: enabled is draw state, NOT geometry/cache key.
  Remove generator early-return on !enabled or disabled→enabled can cache an
  empty field. Both extraction paths already check enabled.
- Need review lifetime/detach GPU-idle behavior, native direct test-run path
  (may bypass ScriptedSceneLayer), all delegated hunks, no duplicate graph
  material/fallback. Then migrate Fox off24glTF batches into one Grass entity
  plus shared exclusion surface asset and verify editor/native/browser.
- Main gallery service remains running; own managed tab currently at ruins,
  journal closed, slider.6/toggletrue. No git mutations.

## 18. Latest live feedback, settings-save defect and incremental delivery

USER PRIORITIES added during execution:
- Current grass is too sparse (actual screenshot). Do not accept the current
  sparse field as visually complete. DenseGrass has now authored a16-blade
  alpha-masked tuft PNG on existing crossed triangles, spacing.6, width.95,
  height.24–.44. Source changed; NOT yet published/visually accepted.
- Ruins look like unrelated test props (actual screenshot). Parent acknowledged
  that the arch/columns/floating cube/sphere were not a believable sanctuary.
  SanctuaryCourtyard agent completed paving + collision/exclusion, one taller
  traversable entrance instead of duplicate character-shaded gate, perimeter
  ruins and visibly supported hinge relic. Resting sphere location preserved.
  Agent moved character shading onto Explorer and repurposed coastal flat panel.
  All changes require parent hunk review + real traversal/projectile/UI proof.
- User explicitly AUTHORIZES commits and pushes AS WORK PROGRESSES. Make
  verified coherent milestones, not one final all-at-once commit. Identity and
  remote verified: Alex Mollard <47443168+AlexMollard@users.noreply.github.com>,
  master, origin https://github.com/AlexMollard/AetherCore.git.
  master was already ahead1 at e47190f8 (prior production3D qualification).
  No new commit/push has happened yet; do this after next verified milestone.
  Never include unrelated .codex/config.toml or .omp/mcp.json.

Built-in grass acceptance already OBSERVED on older sparse candidate:
- bg_81 native build passed; CTest bg_82 passed4/4 including new generator tests.
- publish-builtin-grass.json PASSED, live at18989. Browser shows509meshdraws:
  initial167visible/304frustum/38distance culled,1298x730 actual framebuffer.
- Real keyboard A540→Pond E, then A277/W146→Coastal E. Visited journal4/5;
  actual mouse orbit turned toward ocean. Runtime running through25628ticks,
  no diagnostics; coastal45visible/411frustum/53distance. Saved PNGs under
  logs/fox-showcase/builtin-grass-{trailhead,pond-journal,coast-journal,coastal-view}.png.
- bg_84 rebuilt safe native grass upload/retirement; bg_86 native500tick scene
  PASSED (native-builtin-grass.json, jobc51b7ef5-51d3-4781-83d9-0e8b69749d11).
- Native GPU uploads now run via existing IEngineRuntime::RunExclusive(Drain).
  Cache is keyed/shares immutable chunks. Unused native keys retire only after
  draining render packets + GPU idle; no unbounded live-edit history.
- Old24 project grass GLTF source files have been deleted. Generated .mesh
  caches may still exist (ignored) and should be cleaned with other scaffolds.
- Native graph fixture was missing its compiled shader overlay. Existing
  tools/shaders/web_materials.py --native-output-dir
  projects/FoxShowcase/Builds/Intermediate/shaders cooked it successfully:
  logs/fox-showcase/graph-native-cook.json (bg_87).

New verification-discovered ROOT BUG:
- First owned native editor launched, reflected Grass was correct (id454).
  Screenshot native-builtin-grass.png was BELOW terrain in edit camera, so it
  does NOT visually prove grass rendering. Need Play/maincamera or correct
  editor camera before accepting native visuals.
- Closing that editor rewrote ProjectSettings.toml: string-array
  publish.web.additional_assets lost all quotes, making TOML unreadable;
  bloom.035→.04 and grain.015→.01 were rounded. This is OUR workflow damage,
  not user changes. Parent restored quoted array and exact lighting values.
- ProjectTomlPersistence agent fixed typed TOML persistence through TextIni/
  TomlConfig + float precision in EngineSettings. Parent inspected code,
  simplified to paired decoded/serialized maps and SetValue, deleted now-dead
  string-type guessing, and fixed dirty tracking for scalar type changes.
  m_serialized_values was renamed using LSP. Real parse/value tests added;
  old quote-style/source-string assertions were removed, not repinned.
- Pending bg_88 builds Editor/GameRuntime/EngineTests/AssetPacker/aether-project/
  WebMaterialCompiler with these fixes and latest shader wind flag correction.
  Await actual delivery; do not poll. Then run tests and actual open/close
  smoke proving metadata + float values survive, before first commit/push.
- First owned editor receipt106e11d2c974463387019437052c3d7e was closed cleanly.
  Relaunch receiptae63227ad33045d797ed675f69521bc9 failed at unreadable TOML
  and exited (cleanupRequiredfalse); port8797 has no listener. No native editor
  currently open. Main18989 service and own livingTab remain open.

Browser grass lifetime follow-up:
- GrassSlotRetirement agent added explicit WebGpuDevice queue-completion mesh
  retirement and slot reuse; unreferenced grass keys no longer remain resident.
- CPU model/primitive metadata is created at original scene-load tick, while
  their GPU uploads wait for retired grass budget. WebGameplay Draw now gates
  on WebScene::RenderReady→WebModels::RenderReady, withholding partial frames.
  Parent must review all final hunks and smoke true3/4-scene cycling that would
  exceed the old64MiB cache. No builds/tests run on these latest browser edits.
- Do not defer whole CPU scene Apply or loosen parity tolerances.
- Shared wind now allows ordinary lit vegetation but still excludes nonprelit
  object-lit/character actors; preserves old prelit behavior.

NEXT: receive bg_88; run settings/generator/full suite, actual editor save smoke,
commit/push first verified settings milestone. Publish denser grass/courtyard
after source review, exercise real player through arch + sphere + relic + UI,
complete remaining Woodland site/lighting/native visuals, validate grass
retirement with private throwaway cycling project, run affected browser suite.
Update docs/web-export.md + this plan, remove throwaways, close own windows,
keep18989 running and commit/push verified remaining milestones.

## 19. Superseding grass direction: Tsushima-inspired low-poly meadow

User rejected the denser crossed-card revision too. The actual published view
still resembles individual broad clumps in rows, not a continuous grass field.
Do NOT accept that revision. New target: slender curved low-poly blades, dense
coverage, coherent travelling wind, broad green/gold variation, player bending.
Use the visual inspiration, not a claim of AAA implementation/performance parity.

Shared implementation contract, agreed before splitting exclusive file owners:
- GrassComponent defaults: spacing .14m, chunkSize4m, heightRange .35-.85m,
  bladeWidth .035m, lodDistances[10,26], drawDistance44, windStrength1,
  optional surfaceMaskfalse (terrain vertex R=density, G=height, B=dryness).
- GrassField public API is already defined in GrassMeshData.hpp: Create/Load,
  SelectChunks(local camera, reused vector), GenerateChunk(key), DistanceFade(lod).
  Move-only prepared field owns immutable terrain/exclusion spatial bins, loaded
  once. Remove obsolete GenerateGrass/LoadGrassMeshes and migrate every caller.
- GrassChunkKey is global integer x/z + lod; every layer uses global4m chunks.
  Disjoint nested density layers: lod0 finest-grid cells except both-even;
  lod1 even/even cells except both-multiple-of4; lod2 multiples-of4.
  Root jitter/hash/shape use canonical finest-grid coordinates so roots remain
  identical across boundaries. Each layer fades .75*distance..distance.
  This avoids LOD replacement popping and per-blade metadata hacks.
- Select camera-local chunks only, snap selection centre to1m, add1.5m safety
  margin, sort near-first, retain unchanged chunks and prepare a bounded batch.
  Do not regenerate/load terrain or replace the entire field on camera movement.
- Blades use real tapered curved geometry (5vertices/3triangles), not crossed
  alpha cards. UV2.x is anchored height fraction; UV2.y is actual blade height.
  Opaque vertex-tinted material, no clump texture, broad deterministic patch
  variation, darker roots/golden-green tips, varied height/lean/yaw.
- Generated-grass material flag32768; distanceFade.z stores grass wind strength.
  Native material source fields grass / grassWindStrength are runtime-derived
  from the Grass descriptor. Existing non-grass rendering stays unchanged.
- Shared wind helper contract:
  SharedGrassDisplacement(time, look, worldPos, anchorWeight, bladeHeight,
  windStrength, float4 benders[8], float4 benderParams[8]).
  Use coherent slow gust waves plus restrained flutter, anchored roots.
- New GrassBenderComponent.hpp already defines radius/strength and
  GrassBenderFrame (8positionRadius vec4,8parameter vec4; strength in x).
  Select nearest8 on GAME thread, unused radius0; append grassBenders[8] and
  grassBenderParams[8] to native/Web uniform contracts. Native packet carries
  immutable GrassBenderFrame; never access ECS on render thread.
  Player authors a Grass Bender; displacement bends away and recovers smoothly
  through radial falloff. Inflate chunk bounds for wind and interaction.
- Native streaming owner: native caches/layer/WorldRenderer/material/GPU packet/
  reflection/bender extraction. Web owner: WebModels/WebScene/WebGPU queued
  upload/retirement/uniforms + resources/web/mesh.slang and capability preflight.
  CPU geometry owner: GrassMeshData.cpp + generator tests.
  Parent: shared headers above, native Slang/shared wind, Fox assets/settings,
  docs, full builds and real native/browser acceptance. No mid-wave checks.
- Keep64MiB/1024slot Web limits. Retire outgoing chunks in batches after queue
  completion; reuse slots. Streaming must not block world rendering or deadlock
  callbacks while waiting for uploads. Ordinary scene model readiness remains
  gated. Share same field/chunk geometry across identical descriptors.

Verified delivery since section18:
- e47190f8 prior qualification milestone pushed successfully.
- 1dd64b02 Preserve project metadata and lighting precision on save committed
  and pushed. Current native build passed; CTest4/4 passed77.27s. Actual editor
  open/play/close preserved all8asset paths, bloom.035 and grain.015:
  logs/fox-showcase/settings-editor-roundtrip.json.
- publish-dense-courtyard.json PASSED. Live18989 view running, no diagnostics,
  514meshdraws,172visible/304frustum/38distance;1298x730 framebuffer.
  Screenshot dense-grass-actual.png is evidence of WHY grass needs replacement,
  not visual acceptance. Courtyard traversal and native grass still unaccepted.
- Private lifetime publisher FAILED verification after178s, verify.log empty:
  logs/fox-showcase/grass-retirement-publish.json. Inspect detailed verifier
  artifacts; do not assume cause. Private fixture .temp/fox-grass-retirement-11e74
  cycles3scenes/24loads, animated clips attached before GPU readiness, duplicate
  shared fields and disabled/enabled sharing. Reuse for new streaming proof.

Final acceptance remains real browser/native camera views, dense meadow at
ground/player level (not isolated spikes), visible travelling wind and player
bending, movement across patch boundaries, safe multi-scene retirement, existing
sphere/arch/relic/UI interaction proofs, restrained lighting, all affected suites.
Keep main18989 running, remove own private scaffolds after proof, close owned
editors/tabs, commit and push verified engine/world milestones as work progresses.

## 20. Geometric meadow live; final native/resource acceptance in progress

Supersedes the crossed-card implementation. Main18989 currently serves
publish-curved-meadow.json PASSED (bg99). Actual browser screenshot
curved-meadow-actual.png shows a dense continuous blade meadow. Running at252ticks:
1104meshdraws,368visible/725frustum/11distance culled,1298x730 backing resolution,
no diagnostics. Later real walking maintained running through61533ticks with no
diagnostics. grass-player-bending.png shows grass leaning away from the player.
Eight real frames grass-wind-0..7.png: grass-only crop[80,480,480,715] changed
mean10.29pixel values;22.4%pixels changed>20 (away from actors/UI).

Implemented:
- Prepared GrassField, 5vertex/3triangle curved blades, nested disjoint density
  layers10/26/44m, per-layer dither fades, conservative frustum bounds.
- Terrain COLOR_0 now paints density/height/dryness; renderer vertex_color=false.
  Shorter fox habitat, broad green/gold growth variation, no crossed clump texture.
- Shared wind/player bending + backlighting, grassflag32768, distanceFade.zwind.
- Native typed packet commands create materials/meshes on render thread.
  Async staging uses TransferManager tickets; producer never reads unready meshes;
  pending transfers retain ownership, resource retirements happen after packet/
  GPU lifetime, dropped packets defer commands, shutdown flushes pending work.
- Required graphics transfers are distinct from optional grass uploads; completed
  tickets still establish GPU visibility. No per-batch device-wide GPU idle.
- Native selection includes active preview/RT cameras; per-view nearest8benders
  and RT elapsedTime are copied into immutable packets. Orphan runtime components
  are removed before retirement. Source-only grass meshes invoke bake hooks.
- Buffer registry facade operations/AdvanceFrame share a recursive lock.
  TextureRegistry/MaterialRegistry bounds checks moved inside their mutexes.
- Web queues4chunks/frame, retains resident chunks, physical/temporary logical
  slots preserve CPU attachment timing, normal model byte capacity reserved by
  safe grass retirement, world rendering continues for deferred prefab uploads,
  manual-frame extraction continues while streaming without ticking simulation.
  WebMeshInstance208bytes = MeshGpuInstance192 + vec4distanceFade (xyfade,zstrength).
- Scaled selection now returns Expected<void>, conservative inverse scale and
  independent local snap allowance. Wind strength is NOT in geometry key.

Parent review fixes NOT YET BUILT after bg102:
- Native pipeline registry's opaque state pointer could be invalidated by
  vector growth; changed PipelineSlot storage to deque and removed reserve.
  Added tests/vulkan/ResourceRegistryLifetimeTests.cpp to preserve pointer lifetime
  across513registrations, then destroy/drain real registry entries (nullGPUobjects).
- Facade texture/pipeline creation/lookups and pending/debug queries now use the
  same lock too, not only buffers. Vk pipeline compilation stays outside lock,
  registration locked. ResourceRegistry header comment describes facade ownership.
- LSP-renamed5shadowing locals in GrassMeshData.cpp to remove new C4456warnings.
  LSP cross-file references for BufferMutex returned only declaration despite
  knownCPPdefinition/calls; reported xd://report_issue. Do not trust that index.

Checks:
- bg101 initial native integration build+clangd regeneration PASSED217s, predates
  final registry/per-view fixes.
- bg102 later native all-target build and CTest4/4 PASSED207.99s (tests76.12s),
  but predates parent deque/additional-lock/test/rename changes above.
- bg103 affected browser checks PASSED326.12s:
  logs/fox-showcase/meadow-regressions/report.json.
- bg100 bake-all: terrain1baked,30up-to-date. Asset source positions unchanged;
  appended growth colors only. Native terrain triangles8198allup,normalsagree.

Owned live native editor:
- Receipt logs/agent-editor/8b59cb82d4bb44fc9e739fd2b37f520a/launch.json,
  PID100124, namefox-meadow-final, port8797. MUST CLOSE discardChanges:true.
- Play ran ~929seconds/61892frames,40fps,610entities,consoleERROR0.
  native-curved-meadow-playing.png proves native dense grass renders.
- Grass entity was458, Explorer459 DURING THAT PLAY session; stop restores
  snapshot and may remap IDs, so rediscover before editing.
- get_component Grass exposed all new fields correctly. setwind_strength.5
  succeeded, removeGrass succeeded and field disappeared safely.
- editor_undo in PLAY returnedqueuedtrue but Grassdidnotrestore. Native owner
  is investigating intentionalPlayblock vsnewundo bug. Parent then STOPPED,
  editor is currently EDITING; normal edit-mode undo still needs verification.
- Native image has missing mountain terrain/floating outcrop props unlikeWeb.
  Prior section already noticed odd native background; NOT parityqualified.
  Source terrain8198triangles allup andnormalsagree. Reversible material
  doubleSided=true probe onterrainid1 changednothing; discardedbyStopPlay.
  Native owner investigating only if cheap. Do not claim scene pixelparity.
- MCP results through Eval: details.xdev.inner.structuredContent holds full data;
  text may be truncated. Entity listing retained in JS entityListing (oldPlay).

PRIVATE LIFETIME FAILURE STILL ACTIONABLE:
- .temp/fox-grass-retirement-11e74 source, scripts now dwell240ticks per scene,
  24loadsA/B/C, .14spacing/4mchunks, duplicate identical fields, Cfirstdisabled.
  This forces full residency; old45tick fixture was insufficient.
- bg104 publisher PASSED73.28s:
  logs/fox-showcase/grass-streaming-lifetime-publish.json; short verifier only.
- Real browser long smoke FAILED atGrassBcycle2 tick386:
  BROWSER_GAMEPLAY "Browser grass desired residency exceeds the64MiB mesh upload budget".
  Screenshot grass-lifetime-cycle-24.png is FAILURE evidence, not acceptance.
- WebGrassStreaming agent revived to diagnose actual bytes/accounting/cache vs
  genuinely overbudget configuration. NEVER raise64MiB/1024caps or silently
  starve, and do not reduce fixture density as workaround. Await agent result.
- Private server grass-lifetime-proof-http-18991 PID84704 uses python -mhttp.server
  at .temp/fox-grass-retirement-11e74/Builds/Web/GrassRetirementProof.
  serve-preview.py rejects a standalone package as gallerybuild; directHTTPworks.
  JS grassLifetimeTab namedgrass-lifetime-proof is alive, failed state.
  Keep main gallery-projects-18989 running; stop only own private18991 atcleanup.

Courtyard real proof and latest source correction:
- RealR respawn,A132,W812 reachedsphere/court; Eopenedjournal, Fspawnedstone/chime,
  Gresetstone,Escclosed; runtime running61533ticks,0diagnostics,scheduledSounds2.
  Screens courtyard-approach/journal/relic-launch/clear-view.png.
  Realpointer UI togglefalse→true; slider clicked but same.55value (notnewproof).
- Visualfound frame presented side-on and bricktexture stretched into wood-like
  poles; it hid hangingweight. Parent NOW changed Interactions.scene.toml:
  piersx[-3.7,-1.9],z-16,scale[.28,2.2,.36], matchingboxes; axlealongX,
  hingeaxisX. Removed brickalbedo/normal frompiers andrestingball.
  F nowspawns(-2.8,1,-14.65),velocity(0,1,-3.5), aimedthroughopening.
  These latest courtyard/script changes NOT YET PUBLISHED/SMOKED.
  Need actualarchwalkthrough, visiblehingecontact, lastWoodlandsite5/5.

Current running agents: WebGrassStreaming diagnoses real lifetime failure;
NativeGrassStreaming investigates undo semantics/cheap native terrain cause.
No background builds active now. Native receipt editing, both browser tabs live.
Pending: finish ownership/lifetime, rebuildparentnativechanges+newtest, actual
nativeauthoring+scene-switchsmoke, finalpublishcurrentcourtyard, completebrowser
tour/lighting, update stale docs/web-export.md (stilloldfreecam/Run wording),
graphifyupdateifpresent, cleanupprivateassets/services/windows, commit/push
verified engine and world milestones. Only1dd64b02settingsmilestone committed
thissession sofar; uncommitted engine/world remain. Never stage unrelatedconfigs.

## 21. Verified engine milestone; illustrated UI replaces the rejected plain HUD

This section supersedes the unfinished-state statements in section 20.

- Engine milestone `782d5fbd` is committed and pushed after `24cefcfb`.
  Settings persistence milestone `1dd64b02` was already pushed.
- Final native build, clangd configure and CTest passed (bg106; 4/4 CTest).
  Final affected browser checks passed: logs/fox-showcase/final-meadow-regressions/report.json.
- The long private GPU lifetime failure was historical normal-model retention,
  not excessive active grass: active grass plus one rig fits the unchanged
  64 MiB budget; two historical rigs did not. ClearEntities now retires model
  and primitive GPU slots as well as grass. Real 24-scene, 10,682-tick browser
  proof passed with no diagnostics: grass-lifetime-long-smoke.json.
- Native grass authoring/removal and edit-mode undo passed. Undo is deliberately
  refused during Play with an honest reason. Both owned editors closed cleanly;
  no claim of full-world native/Web pixel parity (native terrain discrepancy).
- Final courtyard, visible relic motion, E/F/G/Escape, arch traversal and all
  five journal sites were exercised in the actual browser. Native 500-tick
  playable check also passed after the latest camera/HUD changes (bg111).
- Player body now uses the same ordinary PBR lighting as its joints: removed
  the inconsistent legacy character/object-lit effects.
- Camera has no trailing position interpolation or automatic recenter. Moving
  native snapshot has exact orbit offset; stopped browser static-background
  comparison found best offset (0,0), mean pixel difference 0.288.
- Compact HUD revision was published (bg110) and visually exercised. Fresh
  localhost origin proved immediate 1/5 progress in HUD and modal, real pointer
  slider .55 -> .30, toggle false -> true, and Escape returning to exploration.
  USER REJECTED ITS PLAIN QUADS/TEXT: this is not final visual acceptance.

Current UI design and execution:

1. Replace plain plates with an engraved circular sunset/compass crest,
   feathered dark backing, distinct keycaps and shaped contextual plaque.
2. Show five actual discovery markers tied to the persisted visited bitmask;
   do not invent health/stamina or show fictional navigation telemetry.
3. Replace the modal rectangle with an illustrated open leather/paper journal:
   notes on the left, accurate illustrated world map on the right, existing
   real slider/toggle and keyboard interaction preserved.
4. Agent JournalArtwork owns only new PNGs under assets/textures/ui/. Parent
   owns scene/prefab/Rust integration, visual/browser and native verification.
5. Publish current source, inspect both HUD and open journal in the browser,
   exercise controls/progress/Escape, then update this plan with actual results.

Still finish: illustrated UI; docs/web-export.md; required graphify update;
remove unused old grass tuft PNG and private smoke scaffolds/services/windows;
keep gallery18989 running; commit/push project/gallery milestone with explicit
paths only. Leave unrelated .codex/.omp, website work and TestingProject saves.

## 22. Illustrated UI and final live acceptance

Supersedes the in-progress UI/delivery notes above.

- The HUD now has an engraved sunset crest, feathered backing flush to the
  top-left canvas corner, five discovery seals and separate movement keycaps.
  Discovery numbers reflect the saved bitmask, not decorative completion.
- The rejected heavy bottom-right plaque and empty circular key inset are
  removed. A small illustrated leather journal icon accompanies tightly
  grouped location/action lines, a subtle rule and no enclosing border.
  Near, away and open-journal states were exercised and captured.
- Field notes open a real illustrated two-page book with paper edges, leather,
  stitching, a world atlas and the existing interactive slider/toggle.
  Footer hints and page numbers sit inside the paper margins.
- The text-centering fault was authored enum data: vertical alignment accepts
  `middle`, not `center`. All keycap/seal labels now use the correct enum;
  adjacent action labels share their row bounds. Actual high-DPI screenshot
  ink centres match keycap horizontal centres exactly and vertical centres
  within one pixel (WASD, Shift, Space).
- The map no longer paints all inland terrain as sand. Real beach glTF
  triangles clipped above sea level y=-0.1 project to x[-56,-46], z[-80,80]:
  1,600 square metres, 6.25% of the 160x160 map. Sea, narrow beach, meadow,
  woods and mountains have distinct washes; sites retain world coordinates.
- Gallery root18989 visibly exposes Fox sunset world as Playable with the
  current third-person controls, not the obsolete free-flight instructions.
  The gallery reports81 examples and0 being worked on at this checkpoint.

Verification:

- Normal Web pipeline publishes successfully, including relocated package,
  production-boundary and real hardware-browser/iframe checks:
  logs/fox-showcase/publish-illustrated-journal.json.
- Final native500-tick movement/collision/journal/F/G/Escape sequence passes
  at its authored1/30-second step. Latest behavioral run:
  logs/gameplay-jobs/222a8c5e-69d1-4fed-bf16-4f3e28104f8a/
  and logs/fox-showcase/native-illustrated-journal.json.
- Real browser pointer slider .55 -> .25, toggle false -> true; Tab selected
  the slider and Right changed .25 -> .30. Three open/close cycles returned
  to the same168 HUD glyphs each time, without orphan modal text.
- Separate saved origins show1/5 and5/5 correctly in both seals and journal.
  Actual backing1420x799 at CSS1136x639, DPR1.25; no runtime diagnostics.
- Current screenshots: final-grouped-context-near/journal/away.png,
  final-one-site-progress.png, final-flush-corner-hud.png and
  final-flush-journal-controls.png under logs/fox-showcase/.
- Web publish coverage was valid producer data missing from report-v1 schema.
  Typed optional coverage and finite numeric/boolean validation now preserve
  strict rejection of malformed/unknown records.27 Web-publish tests and
  2 actual HTTP gallery-routing tests pass. Fix pushed as a500bf07.
- Required graphify code updates completed after the code cutover. The
  managed aethercore-scene-ui-authoring procedure resolves and is scoped
  separately from gallery authoring and native ImGui verification.

Cleanup and delivery:

- Removed unused old clump texture, crude map, unused pin variants, superseded
  plaque texture and the private lifetime fixture. Private18990/18991 servers
  stopped; keep gallery-projects-18989 alive. Owned native editors are closed.
- Source text is normalized to required CRLF. Generated bakes, packages,
  Cargo targets, graph output and logs remain ignored.
- Deliver the world source, catalogue/routing changes, web-export docs and this
  plan together. Leave unrelated .codex/.omp, website work and TestingProject
  saves untouched. Engine milestone782d5fbd and report milestonea500bf07 are
  already pushed.
- No whole-world native/Web pixel-parity claim: native mountain-terrain
  discrepancy remains outside this browser-world acceptance. Also, current
  ResolveCanvases does not implement reference-resolution scaling; do not
  claim arbitrary-small-viewport responsiveness from its authored enum.
