# WebGPU and Vulkan game parity

The requested destination is the same authored game, including full 3D and all
engine effects, running locally in the browser after export. Rust and C# remain
supported. A successful build or a simplified browser scene does not establish
that destination. The current production profile remains `webgpu-2d-v1` until
its replacement has real game and visual qualification.

## Architecture

Keep authored scenes, mesh generation/cooking, material semantics, animation,
gameplay and render extraction shared. GPU code consumes owned frame packets,
never the ECS. Share Slang shading/effect math; isolate resource access behind
target-specific bindings. Vulkan keeps its native optimizations; WebGPU uses
explicit buffer/texture bindings and ordered passes. Do not emulate device
addresses as unvalidated integers or silently discard unsupported content.

Parity means equivalent features and measured visual results at identical
camera, resolution, simulation tick, settings and seeds. Exact byte equality
across GPUs is not assumed. Every comparison records tolerances, feature
coverage, driver/browser identities and rejected unsupported requirements.
Performance and memory budgets are measured separately from visual correctness.

## Delivery sequence

1. Shared CPU geometry and address-free mesh frame data, indexed WebGPU draws,
   depth attachment/lifetime, perspective cameras and a bounded real browser
   fixture. Preserve existing 2D presentation and native primitive bytes.
2. Same cooked models, textures and materials: static multi-primitive imports,
   skinning/animation, vertex colors, UVs, alpha masks/blends, tangent space,
   texture color spaces, linked material overrides, prefab/include expansion.
3. Shared lighting and HDR path: directional/point/spot/object/baked lighting,
   ambient/IBL, sky/backgrounds, shadow cascades/local/contact/blob shadows,
   darkness volumes, 2D light maps and occluders, shared exposure/tonemap.
4. Complete visual stack: GTAO, SSR, depth of field, volumetric/height fog,
   bloom, antialiasing/upscaling and color grading; particles, foliage, water,
   procedural materials, gems, collectible sheen, character lighting, light
   shaft cards; custom/material-graph shaders with explicit portability errors.
5. Game/runtime parity: 3D physics and queries, joints, ragdolls, animation
   events, scripted prefabs and scene transitions, input/audio/save/UI semantics
   for both languages. Rendering a static mesh is not a full 3D game export.
6. Production profile and distribution: target-aware asset/shader reachability,
   capability/limit preflight, package manifests, Rust and C# browser exports,
   relocated/iframe tests and native-vs-browser visual qualification fixtures.

The sequence is not permission to claim all effects or 3D exports at step 1.
Unsupported features keep failing explicitly until their implementation and
qualification land. No runtime streaming or server-side rendering substitute.

## Constraints

- Preserve native Full/UiShell initialization and GPU teardown order.
- Preserve existing C#/Rust desktop and Web 2D builds.
- Retire all frame resources after GPU completion; resize must not invalidate
  resources referenced by outstanding work.
- No automatic CI triggers, git hooks, dependency edits under build trees, or
  unsafe browser flags. Qualification runs hidden/owned and closes its processes.
- No commits this turn: repository policy requires a commit request in the
  current user message. Changes remain available for review.

## Verification

Start with shared geometry/layout/bounds/normal-transform tests and a real
hardware WebGPU depth/resize/teardown fixture. Add same-scene native/browser
captures for each shading feature, including known intentional mismatches that
must fail comparison. Keep evidence for one feature separate from claims about
another. Production release requires every item above, both languages and real
relocated browser packages. Cross-browser/device support is reported only for
tested configurations.
