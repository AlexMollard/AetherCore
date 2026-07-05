# Viewport Picking, Selection Highlight & Transform Gizmo — Design

- **Date:** 2026-07-05
- **Status:** Approved
- **Spec:** 2 of 3 (follows Inspector & Entity List overhaul; precedes Scene Serialization)
- **Owner:** Alex Mollard

## 1. Motivation

Spec 1 delivered the outliner + live inspector around the shared `SceneSelection` service, designed so this spec only has to *write* into it. Selecting entities still requires finding them in a tree; the viewport is display-only. This spec closes the loop: **click an entity in the viewport to select it, see the selection outlined in-world, and manipulate it directly with a transform gizmo (ImGuizmo — added to scope 2026-07-05).**

## 2. Goals

1. **Pick**: screen ray from a viewport click → nearest entity → `SceneSelection` (plain = select, Ctrl = toggle, miss = clear). Camera drags never select.
2. **Highlight**: selected entities outlined with world-space wireframe boxes through the existing debug-line renderer; primary brightest; brief pulse on selection change (motion-only whimsy).
3. **Gizmo**: ImGuizmo translate/rotate/scale on the primary selection, W/E/R modes, Local/World, Ctrl-snap — edits flow through the same apply path as the inspector's transform drawer (children propagation + true physics teleport).

## 3. Non-goals

GPU id-buffer picking, outline post-process shader, das raycast bindings, double-click camera focus, multi-select group pivots, undo (own spec). Skinned meshes pick/outline via bind-pose AABB (animation not baked into bounds) — accepted.

## 4. Picking

- **Ray**: `BuildCameraRay(invViewProj, ndc01, cameraPos)` (new `utils/Ray.hpp`); NDC from the viewport image rect the panel already computes (`imageMin/imageMax`); projection at render-target aspect — identical matrices to the renderer, so letterboxing and aspect modes are handled by construction. Conventions (Vulkan Y-flip, depth range) are pinned by a doctest round-trip against the real `Camera` math.
- **Hybrid test, nearest wins**:
  - *OBB sweep*: for every `MeshComponent+TransformComponent` entity with usable bounds, inverse-transform the ray and slab-test the LOCAL AABB (`Mesh::GetAABBMin/Max`) — exact under any affine transform; covers body-less entities (gallery cubes, orbs, fox meshes).
  - *Physics refine*: `PhysicsSystem::CastRay`; the hit body resolves to an entity by scanning `RigidBodyComponent` handles (no reverse map exists; click-frequency makes O(n) fine). Closer hit wins.
- **Primitive bounds**: primitive meshes must carry closed-form local AABBs (verified/fixed in the plan) or gallery cubes would be unpickable.
- Clicking a mesh child (e.g. a fox primitive) selects the child — it owns the material/mesh the inspector edits; the parent is one click away in the hierarchy drawer.

## 5. Selection highlight

Per selected entity: transform the 8 local-AABB corners by `localToWorld`, emit 12 `AddDebugLine` edges into `AetherCore::GetPendingDebugVertices()` (the DevTools/LightingPanel idiom) from `DebugLayer::OnUpdate`. Primary = gold, secondaries dimmer; brightness pulses ~0.5 s after `SceneSelection::ChangeSerial` changes. Entities without bounds get a small marker box at their position. Gated on `IsDebugRenderingEnabled()` like the light gizmos.

## 6. Transform gizmo (ImGuizmo)

- Vendored via CPM, compiled into the existing `imgui` target; `ImGuizmo::BeginFrame()` opens `DebugLayer::OnImGui`.
- `ViewportPanel`: `SetDrawlist` + `SetRect(imageMin, imageSize)` + `Manipulate(view, unflippedProj, op, mode, primary.localToWorld)`. The Vulkan `proj[1][1] *= -1` is undone for ImGuizmo (GL conventions); orientation verified at RUN.
- Ops: W/E/R keys (viewport hovered, no text input) + toolbar icon buttons; Local/World toggle; Ctrl-hold snap (0.5 / 15° / 0.1).
- **Shared apply path**: `ApplyWorldTransform(context, world, entity, mat4)` — extracted from the inspector's transform drawer — is the single place edit semantics live: children follow verbatim, physics bodies get `SetPosition/SetRotation` + `prev==curr` interp state. The future undo spec hooks here.
- Priority: picking is skipped while the gizmo is hovered/active; ImGui capture shields the camera during gizmo drags (verified at RUN).

## 7. Testing / verification

doctest: slab-test suite (hit/miss/inside/behind/axis-parallel) and the camera project→unproject round-trip. Everything visual is a RUN hand-off checklist in the plan (fox child pick, gallery-cube pick, ctrl-add, sky-clear, drag-no-select, outline pulse/tracking, gizmo orientation + teleport + snap + no-repick).

## 8. File change map

**New**: `engine/utils/Ray.hpp`; `app/debug/ScenePicker.{hpp,cpp}`; `tests/utils/RayTests.cpp`; ImGuizmo dependency.
**Modified**: `app/debug/ViewportPanel.{hpp,cpp}` (pick + gizmo); `app/debug/ComponentDrawers.{hpp,cpp}` (extract `ApplyWorldTransform`); `app/layers/DebugLayer.cpp` (outlines, `ImGuizmo::BeginFrame`); `CMake/Dependencies.cmake`; `tests/CMakeLists.txt`; possibly `engine/mesh/PrimitiveMeshes.*` (closed-form bounds).

## 9. Follow-on

**Spec 3 — Scene Serialization** (unchanged). Future polish: outline post-process, group pivots, das raycast bindings, undo/redo spec hooking `ApplyWorldTransform`.
