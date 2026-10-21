# WebGPU custom material contract

Material graphs use the native graph node emitter and math helpers to generate an
address-free `GraphSurface EvaluateGraphSurface(GraphInput)` function. The WebGPU
adapter supplies explicit mesh, frame, shadow and texture bindings and preserves
the generated native graph's established lighting behavior. A custom material is
never substituted with the standard mesh shader when cooking or loading fails.

The five texture slots are albedo, normal, metallic/roughness, occlusion and
emissive, in that order. `GraphSampleTexture(slot, uv)` samples those resources;
`GraphTexturePresent(input.textureMask, slot)` distinguishes an absent normal map
from a white fallback. Graph inputs carry unmodified UVs and vertex colors,
world-space geometry and source tangent handedness. Graph time uses the frame's
elapsed seconds. Native generated graphs return opaque alpha; this port retains
that contract.

Hand-written native BDA/SPIR-V shaders require an explicit portable surface
adapter at `assets/shaders/<relative-name>.web.slangh`. It defines the same
`EvaluateGraphSurface` function. Additional resource declarations, device
addresses, custom includes and control features outside this contract fail with
actionable diagnostics. This boundary does not translate arbitrary native shader
source automatically.

Cook a project material with:

```text
python tools/shaders/web_materials.py --project <project> --material <material.toml> --output-dir <project>/assets/shaders --report <report.json>
```

The cook requires the CMake `WebMaterialCompiler` target and pinned Slang version.
Graph/node budgets, finite values, shader paths, compiler deadline, fixed bindings
and a 1 MiB WGSL limit are checked. All vertex, fragment, shadow and auxiliary
entries compile before replacing an existing artifact. Compiler failure preserves
the previous artifact. The JSON report records dependency/tool/source hashes and
explicitly distinguishes compilation from GPU execution and visual qualification.

`shaders://effects/Pulse.spv` resolves in the browser to
`project://assets/shaders/effects/Pulse.wshader`. AEWG container ABI 1 has an
88-byte little-endian header: magic, ABI, payload length, flags, FNV-1a checksum,
WGSL SHA-256 and source SHA-256, followed by UTF-8 WGSL. The loader validates ABI,
flags, exact length, UTF-8 and checksum before uploading an immutable shader slot.
SHA-256 fields provide cook provenance; they are not an authenticity mechanism.
At most 128 custom shader slots may be loaded in a browser runtime.

For private same-scene native comparison, `--native-output-dir <shader-overlay>`
also emits the actual native graph shader and compiles SPIR-V with the engine's
default native Slang flags. This output belongs in the private qualification
project's shader overlay. Generated SPIR-V and WGSL containers are not maintained
source assets. Compiled shaders alone do not establish rendering parity; actual
browser execution and a native image comparison remain separate checks. The
production 3D export qualification gate remains unchanged.
