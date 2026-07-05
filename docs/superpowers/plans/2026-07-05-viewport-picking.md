# Viewport Picking, Selection Highlight & Gizmo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans, task-by-task with checkboxes. Spec: `docs/superpowers/specs/2026-07-05-viewport-picking-design.md`.

**Goal:** Click-to-select in the viewport (hybrid OBB/physics ray), gold wireframe outlines on the selection, and an ImGuizmo translate/rotate/scale gizmo sharing the inspector's edit semantics.

**Verification model:** BUILD = `cmake --build build-vs2022-msvc --config Debug --target App EngineTests` (single `--target` list — repeated flags silently drop targets); TEST = run `build-vs2022-msvc/tests/Debug/EngineTests.exe`; **reconfigure (`cmake -S . -B build-vs2022-msvc`) after adding files** (GLOB + VS generator misses them); RUN = hand-off checklist for Alex (GPU app). One commit per task, footer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

---

## Task S1: Ray math + tests

**Files:** create `src/engine/utils/Ray.hpp`, `tests/utils/RayTests.cpp`; modify `tests/CMakeLists.txt`.

- [x] `Ray{origin, dir}`; `bool RayVsAabb(const Ray&, const glm::vec3& mn, const glm::vec3& mx, float& tHit)` — slab method: per-axis inverse-dir (handle ±inf), running tmin/tmax, reject tmax < max(tmin,0); origin-inside ⇒ tHit = 0.
- [x] `Ray BuildCameraRay(const glm::mat4& invViewProj, glm::vec2 ndc01, const glm::vec3& cameraPos)` — unproject (ndc01*2-1) at two depths, perspective-divide, `origin = cameraPos`, `dir = normalize(far - near)`.
- [x] Tests: slab hit/miss/inside/behind/axis-parallel/negative-dir; **camera round-trip** — real `Camera` (free mode, known pos/yaw/pitch), project world point via `GetProjectionMatrix(aspect) * GetViewMatrix()` (Y-flip included), NDC→pixel→`BuildCameraRay`, expect point-to-ray distance ≈ 0 for several points/aspects. This pins Vulkan NDC-y + depth-range conventions.
- [x] Reconfigure + BUILD + TEST green. Commit `feat(utils): ray primitives + camera unproject with doctest coverage`.

## Task S2: ScenePicker

**Files:** create `src/app/debug/ScenePicker.hpp/.cpp`; possibly modify `src/engine/mesh/PrimitiveMeshes.cpp` (bounds).

- [x] **Verify primitive bounds**: confirm cube/sphere/plane/quad/triangle meshes carry non-degenerate `GetAABBMin/Max`; if not, pass closed-form bounds at their `Mesh::Create` call (engine fix in this commit).
- [x] `PickHit{Entity entity{}; float t = FLT_MAX;}` + `PickHit PickEntity(World&, PhysicsSystem*, const Ray&, float maxDist)`:
  - OBB sweep over `View<MeshComponent, TransformComponent>`: skip null/degenerate-bounds meshes; ray→object space via `inverse(localToWorld)` (unnormalized local dir keeps t in world scale when measured via world hit point: compute local tHit, worldHit = localToWorld * localHit, t = distance(ray.origin, worldHit)); track nearest.
  - Physics: `CastRay(origin, dir, maxDist)`; resolve `result.body.value` against `View<RigidBodyComponent>`; closer world distance wins.
- [x] Reconfigure + BUILD. Commit `feat(debug): hybrid OBB/physics scene picker (+ primitive mesh bounds)`.

## Task S3: Viewport click → selection

**Files:** modify `src/app/debug/ViewportPanel.hpp/.cpp`.

- [x] After the InvisibleButton: `hovered && ImGui::IsMouseReleased(0) && !ImGui::IsMouseDragPastThreshold(0, 4.0f)` (and gizmo not hot — S5 adds the guard) → `ndc01 = (mouse - imageMin) / imageSize` → ray from `CameraManager` view + proj at `extent` aspect → `PickEntity` → Ctrl? `ToggleSelection(hit)` : hit ? `Select(hit)` : `Clear()`.
- [x] BUILD. Commit `feat(debug): click-to-select in the scene viewport`.

## Task S4: Vendor ImGuizmo

**Files:** modify `CMake/Dependencies.cmake`; `src/app/layers/DebugLayer.cpp`.

- [x] `CPMAddPackage` ImGuizmo (CedricGuillemet/ImGuizmo, pinned tag, DOWNLOAD_ONLY) — add `ImGuizmo.cpp` + include dir to the existing `imgui` lib target so it shares the ImGui context.
- [x] `ImGuizmo::BeginFrame()` first thing in `DebugLayer::OnImGui`.
- [x] Reconfigure + BUILD (compile gate: header reachable, lib links). Commit `build(imgui): vendor ImGuizmo into the imgui target`.

## Task S5: Transform gizmo

**Files:** modify `src/app/debug/ComponentDrawers.hpp/.cpp` (extract), `src/app/debug/ViewportPanel.hpp/.cpp`.

- [x] Extract `ApplyWorldTransform(LayerContext&, World&, Entity, const glm::mat4& localToWorld)` from `DrawTransform`'s post-edit block (children follow verbatim; physics: DecomposeTRS → YXZ quat → `SetPosition/SetRotation` + prev==curr + scale). Drawer calls it; behavior unchanged.
- [x] ViewportPanel: op state (`m_gizmoOp`, `m_gizmoLocal`), toolbar icon buttons (translate/rotate/scale, Local/World), W/E/R keys when hovered && !WantTextInput; `Manipulate(view, unflippedProj, op, mode, model)` over `SetRect(imageMin, imageSize)`; on `IsUsingAny()` write back via `ApplyWorldTransform`; picking skipped when `IsOver()||IsUsingAny()`; Ctrl-hold snap {0.5, 15°, 0.1}.
- [x] BUILD. Commit `feat(debug): ImGuizmo transform gizmo on the primary selection`.

## Task S6: Selection outlines

**Files:** modify `src/app/layers/DebugLayer.cpp` (+hpp if state needed).

- [x] In `OnUpdate` after `m_selection.Prune`: if `IsDebugRenderingEnabled()` and engine available, per selected entity emit 12 world-space AABB-corner edges (`AddDebugLine`) into `GetPendingDebugVertices()`; primary gold (1.0, 0.72, 0.2), secondaries dimmed; pulse brightness 1.6→1.0 over 0.5 s from `ChangeSerial` change; bound-less entities get a 0.25 m marker box at their transform (or physics) position.
- [x] BUILD. Commit `feat(debug): world-space selection outlines with change pulse`.

## Task S7: Acceptance

- [x] Tick non-RUN checkboxes; update spec if execution deviated (none — primitive bounds already existed, so S2 needed no engine fix; ImGuizmo master keeps sources under src/, reflected in Dependencies.cmake); memory notes.
- [ ] **RUN hand-off (Alex):**
  - Click fox → mesh child selects (outliner pulses, inspector fills); click crimson gallery cube → selects; ctrl-click adds; click sky → clears; camera drag ≠ select; letterboxed aspect modes still pick accurately at edges.
  - Outlines: gold boxes track moving toys/fox; primary brightest; pulse on change; disabled cleanly when debug rendering off.
  - Gizmo: correct orientation (not mirrored); W/E/R + toolbar switch; drag translates/rotates/scales live; dynamic toy teleports without snap-back; fox children follow parent drags; Ctrl snaps; gizmo drag never re-picks or orbits; Local/World behaves.
  - F5 reload: no crash; selection clears/prunes; picking works on respawned scene.
- [x] Commit `docs: tick Spec-2 plan, acceptance notes`.
