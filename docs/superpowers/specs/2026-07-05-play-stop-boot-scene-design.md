# Play/Stop, Boot-from-Scene & Sandbox Migration — Design

- **Date:** 2026-07-05
- **Status:** Approved
- **Spec:** 4 (follows Scene Serialization)
- **Owner:** Alex Mollard

## 1. Motivation

Spec 3 made scenes persistent, but the app still boots by running `sandbox.das on_attach` and simulates from frame zero — there is no way to *set up* a physics scene (stack boxes, arrange dominoes) before letting it run, and scene files are load-on-request only. This spec adds the **editor runtime loop**: boot into a frozen edit mode from a startup scene file, arrange freely, **Play** to simulate, **Stop** to snap back to the arrangement. The sandbox content itself migrates into the scene-file system.

Context that shaped the design: **daScript is planned to be replaced by C# scripting soon**, so per-frame scripted behaviors move into serializable engine data (components + a system) rather than deeper das investment. The script keeps only the interactive seam (player controller, camera, tag creation, toy-spawn inputs) — a small surface for the future C# port.

## 2. Decisions

- **Edit mode freezes physics + scripts + animations** (day/night included). Camera, picking, gizmo and inspector all stay live. Paused physics still flushes pending body descriptors so loaded/created entities get pickable bodies.
- **Boot into edit mode**, `engine.toml [app] autoplay = true` to boot simulating. Startup scene: `[app] startup_scene = "sandbox"`.
- **Play = in-memory `CaptureScene` snapshot; Stop = replace-all `ApplyScene`** of that snapshot — Spec-3 machinery, no new serialization. Restores wait out the async physics step first (`PhysicsSystem::WaitForStepIdle`) — also hardening Spec-3 loads against the same race.
- **Migration covers ALL zones with no lost functionality**: the scripted animations become **behavior components** — `Bob`, `Spin`, `Orbit` (fox patrol), `MaterialPulse` (the Zone-M shared-emissive demo; per-entity instances dedup to one slot per pulse step, preserving the demo's semantics) — driven by an app `BehaviorSystem` that pauses with everything else and serializes like any component.
- **Lights + environment join the scene format** (`[[lights]]`, `[environment]`): zone lights and the sun/ambient/sky rig would otherwise die with the deleted script builders. Replace-all loads clear and re-apply them.
- **Self-healing migration**: on boot, if the startup scene file is missing the script's legacy builders run and the world is auto-captured to `resources/scenes/<name>.scene.toml`; subsequent boots load the file additively after the script prepares camera/tags/player. No hand-authored 100-entity file, no one-time migration ceremony.

## 3. Non-goals

Pause-and-resume mid-simulation (Stop always restores), stepping/frame-advance, das raycast/gameplay binding growth, the C# scripting layer itself, serializing cameras (script-owned until C#).

## 4. Play state

`PlayState` service (app): `Editing | Playing` + the Stop snapshot. `Application::OnUpdate` gates `World::UpdateSystems` (physics/animation/day-night/behaviors) — Editing runs only `PhysicsSystem::FlushPendingOnly`. `ScriptedSceneLayer::OnUpdate` keeps F5 handling but calls `CallOnUpdate` only while Playing. Viewport toolbar: green Play / red Stop.

## 5. File format additions

`[[entities]]` gain optional `bob/spin/orbit/material_pulse` tables; top-level `[environment]` (ambient, sun, sky) and `[[lights]]` (point/spot, full params). Version stays 1 (additive, absent = defaults).

## 6. Verification

doctest: behavior/lights/environment record round-trips + behavior math. RUN: freeze-arrange-play-stop cycle restores exactly; first boot generates the sandbox scene file; autoplay flag; F5 unaffected; Zone-M pulse and fox patrol survive save/load round-trips (they're data now).
