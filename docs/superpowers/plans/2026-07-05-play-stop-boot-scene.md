# Play/Stop, Boot-from-Scene & Sandbox Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans, task-by-task with checkboxes. Spec: `docs/superpowers/specs/2026-07-05-play-stop-boot-scene-design.md`.

**Verification model:** BUILD = `cmake --build build-vs2022-msvc --config Debug --target App EngineTests` (single `--target` list); TEST = run `build-vs2022-msvc/tests/Debug/EngineTests.exe`; reconfigure after adding files; RUN = hand-off checklist. One commit per task, footer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

## Task V1: PlayState + simulation gating

**Files:** create `src/app/PlayState.hpp`; modify `src/engine/physics/PhysicsSystem.hpp/.cpp`, `src/app/Application.cpp/.hpp`, `src/app/layers/ScriptedSceneLayer.cpp`, `src/app/scene/SceneSerializer.cpp`.

- [x] `PlayState { enum class Mode { Editing, Playing }; Mode mode = Mode::Editing; std::optional<scene::SceneDescription> stopSnapshot; bool IsPlaying() const; }` — Application member, registered as a service (Register in ctor-adjacent setup, Unregister on teardown per existing service style).
- [x] `PhysicsSystem::WaitForStepIdle()` (public: WaitForStep) + `FlushPendingOnly(World&)` (WaitForStepIdle then FlushPendingBodies — safe while no steps kick).
- [x] `Application::OnUpdate`: Playing → `world.UpdateSystems(gameDt)`; Editing → `physics->FlushPendingOnly(world)` only (physics via World::FindSystem or service).
- [x] `ScriptedSceneLayer::OnUpdate`: gate `CallOnUpdate` on PlayState (TryGet — default Playing if absent so headless keeps working).
- [x] `LoadSceneFile`: call `WaitForStepIdle` (deps gain `PhysicsSystem*`, HierarchyPanel passes it) before destroy-all.
- [x] Reconfigure + BUILD + TEST. Commit `feat(app): PlayState service gates physics/animation/script simulation`.

## Task V2: Play/Stop UI + snapshot restore

**Files:** modify `src/app/debug/Icons.hpp` (PLAY U+F04B, STOP U+F04D), `src/app/debug/ViewportPanel.cpp/.hpp`, `src/app/scene/SceneSerializer.hpp/.cpp` (extract `ReplaceScene`).

- [x] `ReplaceScene(const SceneDescription&, World&, const ApplySceneDeps&)` = WaitForStepIdle + destroy-all + sceneEntities clear + ApplyScene (LoadSceneFile delegates).
- [x] Viewport toolbar: centered Play (green, Editing only) / Stop (red, Playing only); Play captures `stopSnapshot = CaptureScene(...)`, mode=Playing; Stop: mode=Editing, `ReplaceScene(*stopSnapshot)`, snapshot reset, selection cleared.
- [x] BUILD. Commit `feat(debug): viewport Play/Stop with snapshot restore`.

## Task V3: Behavior components + system + serialization

**Files:** create `src/engine/scene/BehaviorComponents.hpp`, `src/app/systems/BehaviorSystem.hpp/.cpp`, `tests/scene/BehaviorTests.cpp`; modify Application (system registration beside DayNightSystem), `src/app/scene/SceneSerializer.hpp/.cpp`, `src/app/scripting/modules/WorldModule.cpp` (or a small BehaviorModule), `tests/CMakeLists.txt`, EngineTests sources (BehaviorSystem if needed for tests — components suffice).

- [x] Components: `BobComponent{amplitude, frequency, phase, baseCaptured, baseY}`, `SpinComponent{glm::vec3 eulerDegPerSec}`, `OrbitComponent{center, radius, angularSpeedDeg, angleDeg, yawOffsetDeg}`, `MaterialPulseComponent{emissiveA, emissiveB, frequency}` — plain data, engine header.
- [x] `BehaviorSystem` (app): per frame advances each — Bob captures baseY lazily then sets Y = base + sin; Spin adds euler-rate*dt via Decompose/Compose (or matrix rotate); Orbit advances angle, positions on circle around center, faces tangent (+yawOffset), propagates like set_transform (children follow — reuse ApplyWorldTransform? that teleports physics; these are non-physics entities — write transforms directly, mirroring current script math); MaterialPulse lerps emissive by sin and calls `MaterialSystem::SetEmissive`.
- [x] Serialization: optional records in EntityRecord + TOML tables `bob/spin/orbit/material_pulse`, capture + apply.
- [x] das bindings (thin, das-lifetime): `add_bob(world,id,amplitude,frequency,phase)`, `add_spin(world,id,ex,ey,ez)`, `add_orbit(world,id,cx,cy,cz,radius,speedDeg,yawOffset)`, `add_material_pulse(world,id, r,g,b, r2,g2,b2, frequency)`.
- [x] doctest: record↔TOML round-trip for all four; Bob/Orbit math pure-function checks.
- [x] Reconfigure + BUILD + TEST. Commit `feat(scene): serializable behavior components replace scripted per-frame animation`.

## Task V4: Lights + environment serialization

**Files:** modify `src/engine/rendering/Renderer.hpp/.cpp` (getters for ambient/sun/sky if missing), `src/app/scene/SceneSerializer.hpp/.cpp`, `tests/scene/SceneSerializerTests.cpp`.

- [x] `LightRecord` (type, pos, color, intensity, radius, dir, inner/outer, shadow) + `EnvironmentRecord` (ambient, sunDir/intensity/color, skyHorizon/zenith/void, present-flag); SceneDescription gains `std::vector<LightRecord> lights; std::optional<EnvironmentRecord> environment;`.
- [x] Capture from `Renderer::GetPointLights/GetSpotLights` + environment getters; TOML `[[lights]]` + `[environment]`; apply via Renderer add/set APIs; `ReplaceScene`/boot-apply clear lights first (`ClearPointLights/ClearSpotLights`).
- [x] Capture/apply take an optional `Renderer*` in deps (tests pass null — entity round-trip unaffected).
- [x] doctest: light/environment record ↔ TOML round-trip (records only, no Renderer).
- [x] BUILD + TEST. Commit `feat(scene): lights + environment rig serialize with the scene`.

## Task V5: Boot config + startup select

**Files:** modify `src/engine/utils/TomlConfig.hpp/.cpp` (GetString/Set(string)), `src/engine/utils/EngineSettings.hpp/.cpp` (app.startupScene, app.autoplay: parse + defaults + write-back), `src/app/Application.cpp` (autoplay → PlayState), `src/app/layers/ScriptedSceneLayer.cpp` (boot load/auto-generate), `src/app/debug/HierarchyPanel.cpp` (set-as-startup affordance if a config write path exists — else tooltip documenting engine.toml).

- [x] After `CallOnAttach`: if `ReadSceneFile(settings.app.startupScene)` succeeds → additive `ApplyScene` (+ lights/env apply); else → auto-generate `SaveSceneFile(startupScene, CaptureScene(...))` and log loudly.
- [x] BUILD. Commit `feat(app): boot from startup scene file with autoplay flag`.

## Task V6: Sandbox migration

**Files:** modify `src/app/scripting/modules/WorldModule.cpp` (`scene_file_exists(name) -> bool`), `resources/scripts/sandbox.das`.

- [x] Script keeps ALWAYS: mesh handles, tags, player + animations + compile, camera, input bindings, toy helpers.
- [x] Wrap in `if (!scene_file_exists("sandbox"))`: light rig, floor, zones A–E, Zone M, orbs, missing cube, foxes — and attach behaviors at spawn: orbs `add_bob`, missing cube `add_spin`, foxes `add_orbit` (per-fox radius/speed/phase matching today's patrol), the 6 shared-material cubes `add_material_pulse` (colors/frequency matching today's pulse).
- [x] on_update: DELETE orb-bob/spin/pulse/fox-patrol blocks; keep time accumulation only if still used, player/camera/toys.
- [x] BUILD (das validated at runtime — RUN gate). Commit `feat(sandbox): migrate scene content to the scene-file system (self-generating)`.

## Task V7: Acceptance

- [x] Tick non-RUN boxes; memory notes (C# plan recorded); deviations to spec: TomlConfig::GetString was unnecessary (EngineSettings' own text::ParseToml callback handles the string key with quote trim); SceneTransientComponent added for capture exclusion (player duplication guard) — spec §2 amended in spirit; das also gained mark_transient.
- [ ] **RUN hand-off (Alex):** boot → edit mode, world frozen (fox mid-pose, orbs still), camera/pick/gizmo live → arrange toys → **Play** → simulation + behaviors + player controller run → **Stop** → exact arrangement back → Play again reproduces. First boot generated `resources/scenes/sandbox.scene.toml` — commit it; second boot loads it (identical scene incl. lights); delete file to regenerate. autoplay=true boots Playing. F5 reload works in both modes. Zone-M pulse + fox patrol run from data (survive save/load). Toy inputs only respond while Playing.
- [x] Commit `docs: tick Spec-4 plan, acceptance notes`.
