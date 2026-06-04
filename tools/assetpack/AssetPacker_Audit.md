# AssetPacker — Full Code Audit

**Files reviewed:** `MeshProcessor.cpp/hpp`, `PakWriter.cpp/hpp`, `TextureProcessor.cpp/hpp`,
`SpirvProcessor.cpp/hpp`, `MaterialImporter.cpp/hpp`, `BinaryFormats.hpp`, `PakFormat.hpp`,
`main.cpp`, `CMakeLists.txt`

**Overall verdict:** Solid foundation — the architecture, format design, and pipeline structure
are all correct thinking. There are a handful of outright bugs, several correctness gaps that
would show up immediately on real content, and a set of missing features that separate a good
tool from a production-grade one.

Issues are grouped into four tiers: **Critical** (wrong output or UB), **Correctness**
(silent data loss or build-system breakage), **Production Gap** (missing for shipping content),
and **Polish** (minor but worth fixing).

---

## Table of Contents

1. [Critical Bugs](#1-critical-bugs)
   - 1.1 Bounding volumes computed on zeroed data
   - 1.2 Data race in `InitEncoders()`
   - 1.3 Node world transforms not applied when merging primitives
   - 1.4 Non-bone animation channels write sentinel index to disk
2. [Correctness Issues](#2-correctness-issues)
   - 2.1 Extra files missing from the incremental manifest
   - 2.2 Extra file ordering is non-deterministic
   - 2.3 Partial write left on disk after error
   - 2.4 `cgltf_load_buffers` return value ignored
   - 2.5 `ComputeSkeletonHash` called three times redundantly
   - 2.6 `ProcessedFile` struct and comment block duplicated in PakWriter.cpp
   - 2.7 Stale BC5 comment in TextureProcessor.hpp
3. [Production Gaps](#3-production-gaps)
   - 3.1 No mip-map generation
   - 3.2 No tangent generation (MikkTSpace)
   - 3.3 No skinning weight normalization
   - 3.4 Always emits 32-bit indices
   - 3.5 No vertex deduplication / welding
   - 3.6 No binary material format — TOML passes through raw
   - 3.7 `.webp` not dispatched to TextureProcessor
   - 3.8 Sphere bound uses non-tight AABB centroid
   - 3.9 Only one joint/weight set (max 4 influences)
   - 3.10 Flat normal accumulation — no angle-weighted averaging
   - 3.11 BC7 quality not configurable per asset type
4. [Polish / Minor](#4-polish--minor)
   - 4.1 `AppendStr` silently truncates strings > 65535 bytes
   - 4.2 `IsImageExtension` uses `std::set` for a fixed tiny list
   - 4.3 `Stem()` helper duplicated across translation units
   - 4.4 xxhash include strategy inconsistent across TUs
   - 4.5 `animset` animation paths assume a fixed virtual directory layout
   - 4.6 CMakeLists — no explicit compile warning flags

---

## 1. Critical Bugs

### 1.1 Bounding volumes computed on zeroed data

**File:** `MeshProcessor.cpp`, first-pass loop ~line 419

The first pass resizes `allVerts` but never reads positions from cgltf, so every vertex has
`position = {0,0,0}`. `ComputeBounds` then runs on all-zero data, and every mesh gets a
bounding sphere of radius 0 centred at the origin and a degenerate AABB. This breaks
culling, shadow caster selection, LOD selection, and any distance-based system in the engine.

```cpp
// What happens today
std::vector<TempVertex> allVerts;
for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
{
    // ...
    allVerts.resize(allVerts.size() + vertCount); // default-init: pos = {0,0,0}
}
Bounds bounds = ComputeBounds(allVerts); // all zeros — wrong
```

**Fix option A — dedicated position-only pass:**
```cpp
std::vector<std::array<float, 3>> positions;
for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
{
    const cgltf_node& node = data->nodes[ni];
    if (!node.mesh) continue;
    for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
    {
        const cgltf_primitive& prim = node.mesh->primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor* posAcc = FindAttr(prim, cgltf_attribute_type_position, 0);
        if (!posAcc) continue;
        std::array<float, 3> p{};
        for (cgltf_size v = 0; v < posAcc->count; ++v)
        {
            cgltf_accessor_read_float(posAcc, v, p.data(), 3);
            positions.push_back(p);
        }
    }
}
Bounds bounds = ComputeBounds(positions);
```

**Fix option B (simpler):** delete the first pass entirely and compute bounds from
`combinedVerts` after the second pass. You already have all the positions there. Write the
header with dummy bounds, then seek back and patch it, or accumulate bounds incrementally
during the second pass.

---

### 1.2 Data race in `InitEncoders()`

**File:** `TextureProcessor.cpp`

`ProcessFile` runs on `std::async` worker threads. All of them call `ToDDS` →
`InitEncoders`. The guard is a plain `static bool` with no synchronisation — this is a data
race and undefined behaviour under the C++ memory model. The encoders can be partially
initialised for one thread while another is already using them.

```cpp
// Broken — non-atomic read/write from multiple threads
static bool s_init = false;
if (s_init) return;
bc7enc_compress_block_init();
rgbcx::init(...);
s_init = true;
```

**Fix:**
```cpp
static std::once_flag s_flag;
std::call_once(s_flag, [] {
    bc7enc_compress_block_init();
    rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
});
```

---

### 1.3 Node world transforms not applied when merging primitives

**File:** `MeshProcessor.cpp`, second-pass vertex loop

Vertices from every node are read in the node's local space and merged directly into
`combinedVerts`. No TRS chain is evaluated. For a skinned character this is expected
(the skeleton drives the transform at runtime), but for any scene with multiple static
mesh nodes at different transforms — a prop with separately-placed parts, an environment
piece, a multi-mesh vehicle — every sub-mesh will appear at the origin with no rotation
or scale applied. The output is silently wrong.

**Fix:** compute the node's world-space transform before processing its vertices.
cgltf provides `cgltf_node_transform_world` for this:

```cpp
float worldMat[16];
cgltf_node_transform_world(&node, worldMat);
// then transform each position and normal by worldMat before writing into verts
```

For skinned meshes, skip the transform application (or verify the skin's bind-pose handles it).

---

### 1.4 Non-bone animation channels write sentinel index to disk

**File:** `MeshProcessor.cpp`, animation channel loop ~line 647

`remapTable` maps original node index → sorted bone index, initialised to `0xFFFFFFFF`
for non-bone nodes. When an animation channel targets a non-bone node (a mesh node,
a camera, a light), `remapTable[targetNode]` returns `0xFFFFFFFF`, and that value gets
written directly into `ChannelHeaderDisk::nodeIndex`. The runtime will attempt to look
up bone 4,294,967,295 and either crash or silently corrupt the skeleton.

```cpp
// targetNode is a mesh or camera node — remapTable entry is 0xFFFFFFFF
const uint32_t remappedNode = remapTable[static_cast<std::size_t>(targetNode)];
chHdr.nodeIndex = remappedNode; // 0xFFFFFFFF written to disk
```

**Fix:** skip any channel whose target node is not in the remap table:

```cpp
const int32_t targetNode = ToIndex(ch.target_node, *data);
if (targetNode < 0 || static_cast<std::size_t>(targetNode) >= remapTable.size())
    continue;
const uint32_t remappedNode = remapTable[static_cast<std::size_t>(targetNode)];
if (remappedNode == static_cast<uint32_t>(-1))
    continue; // not a bone — skip
```

Also adjust `validChannels` pre-count to exclude these.

---

## 2. Correctness Issues

### 2.1 Extra files missing from the incremental manifest

**File:** `PakWriter.cpp`

`.skel`, `.anim`, and `.animset` files come out of `ProcessFile` as `res.extraFiles`.
They are packed into the `.pak` correctly, but they are never written to `newManifest`.
The manifest only tracks source files by mtime. On the next run, if the source `.gltf`
has not changed, `IsUpToDate` returns `true` even if a derived file was deleted or
corrupted. The build is silently stale.

**Fix:** after processing `res.extraFiles`, add a manifest entry for each one. Since extra
files have no on-disk source path, key them by virtual path and store a content hash.
Extend `ManifestEntry` with an optional `derivedHash` field, or simply always re-derive
when the source changed (mtime already handles that) and mark the extra as "present or
absent" — a simpler fix is just to check that all expected derived virtual paths exist in
the pak, which requires storing their expected paths in the manifest.

---

### 2.2 Extra file ordering is non-deterministic

**File:** `PakWriter.cpp`, `Write()`

`m_files` is sorted alphabetically before processing — deterministic pak output is a
stated goal. But extra files from `ProcessFile` (`.skel`, `.anim`, `.animset`) are
appended to `entries`/`pathData`/`assetData` in the order futures complete, which is
wall-clock dependent. Two identical builds can produce byte-different `.pak` files. This
breaks content caching, signing, diffing, and any CDN that uses content-addressable storage.

**Fix:** collect all `FileResult` objects first (call `future.get()` for all futures into
a `std::vector<FileResult>`), then sort the entire flat list — primary entries and their
extras — by virtual path before writing.

---

### 2.3 Partial write left on disk after error

**File:** `PakWriter.cpp`, `Write()`

When `anyError` is true (a file failed to process), `Write` still writes the `.pak` and
returns `false`. The caller in `main.cpp` uses the return value to set the exit code, but
the partially-populated pak file is left on disk. A downstream build system that doesn't
check the exit code (or that reads stale cached outputs) will silently load the broken pak.

**Fix:** on `anyError`, either:
- Delete the output file before returning `false`: `fs::remove(outPath, ec)`.
- Write to a temp path and rename atomically on success only (preferred — prevents torn
  writes from partial hardware failures too).

```cpp
const fs::path tmpPath = outPath.string() + ".tmp";
// ... write to tmpPath ...
if (!anyError)
    fs::rename(tmpPath, outPath);
else
    fs::remove(tmpPath, ec);
return !anyError;
```

---

### 2.4 `cgltf_load_buffers` return value ignored

**File:** `MeshProcessor.cpp`, line 315

```cpp
cgltf_load_buffers(&options, data, srcPathStr.c_str());
```

For `.gltf` files with external `.bin` buffers, if the `.bin` is missing or on a
different path, this call fails silently. Every accessor read then returns zeroed data
with no error. Positions, normals, and weights all become zero. The mesh writes
successfully but contains garbage geometry.

**Fix:**
```cpp
if (cgltf_load_buffers(&options, data, srcPathStr.c_str()) != cgltf_result_success)
{
    std::cerr << "  MeshProcessor: failed to load buffers for " << sourcePath << "\n";
    cgltf_free(data);
    return {};
}
```

---

### 2.5 `ComputeSkeletonHash` called three times redundantly

**File:** `MeshProcessor.cpp`

`ComputeSkeletonHash` allocates a heap buffer and iterates all bones. It is called at
lines 345, 353, and 703 with identical arguments.

```cpp
result.skeletonHash = std::to_string(ComputeSkeletonHash(bones, remapTable)); // call 1
hdr.skeletonHash    = ComputeSkeletonHash(bones, remapTable);                  // call 2
// ...
setHdr.skeletonHash = ComputeSkeletonHash(bones, remapTable);                  // call 3
```

**Fix:**
```cpp
const uint64_t skelHash = ComputeSkeletonHash(bones, remapTable);
result.skeletonHash     = std::to_string(skelHash);
hdr.skeletonHash        = skelHash;
// ...
setHdr.skeletonHash     = skelHash;
```

---

### 2.6 `ProcessedFile` struct and comment block duplicated in PakWriter.cpp

**File:** `PakWriter.cpp`, lines ~287 and ~315

The "Asset processing dispatch" section comment and the `ProcessedFile` struct definition
appear to have been copy-pasted. The struct is defined once, but the surrounding comment
block reads as if it introduces a second definition. Minor, but it signals the section
needs a tidy-up.

---

### 2.7 Stale BC5 comment in TextureProcessor.hpp

**File:** `TextureProcessor.hpp`

```cpp
// Format selection per source filename:
//   "normal" / "nrm" in stem  -> BC5  (RG, reconstruct B in shader)
```

The implementation uses `BC7_LINEAR` for normal maps, not BC5. The in-code comment in
`TextureProcessor.cpp` even explicitly explains why BC5 was avoided (the engine shader
reads `.xyz` and expects a Z value in the blue channel). The header comment is wrong and
will mislead anyone consuming this interface.

---

## 3. Production Gaps

### 3.1 No mip-map generation

**File:** `TextureProcessor.cpp`

Every texture is packed as a single full-resolution surface with no mip chain.
Without mipmaps:
- Textures alias heavily at distance.
- GPU performance is significantly worse (cache thrashing on distant surfaces).
- Anisotropic filtering has nothing to filter into.

This is arguably the most impactful missing feature for shipped content quality.

**Fix:** after decoding with stb_image, generate a full mip chain down to 1×1 before
encoding each level. Box filter is acceptable for most content; a Kaiser or Lanczos
filter on albedo maps gives higher quality. Each mip level is then BC-encoded independently.
The `DDSHeader.dwMipMapCount` and `DDSD_MIPMAPCOUNT` flag need updating.

```cpp
// Rough structure
std::vector<MipLevel> mips = GenerateMipChain(pixels, width, height, channels);
for (const auto& mip : mips)
{
    const auto blocks = CompressBlocks(mip.pixels, mip.width, mip.height, channels, fmt, bc7Params);
    ddsData.insert(ddsData.end(), blocks.begin(), blocks.end());
}
header.dwMipMapCount = static_cast<uint32_t>(mips.size());
header.dwFlags      |= DDSD_MIPMAPCOUNT;
header.dwCaps       |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
```

---

### 3.2 No tangent generation (MikkTSpace)

**File:** `MeshProcessor.cpp`

When `tanAcc` is null, the code writes a dummy `{1, 0, 0, 1}` tangent for every vertex.
This is not the same as a flat-shading fallback — it means every vertex on a mesh
without exported tangents will have the same hardcoded tangent, producing visibly wrong
normal mapping on any surface that isn't perfectly aligned with the X axis.

Normals get a proper geometric fallback via `GenerateNormals`. Tangents need the same
treatment.

**Fix:** integrate MikkTSpace (single-header, MIT licensed). After positions, normals,
and UVs are read, and before writing `combinedVerts`, run the MikkTSpace pass on any
primitive where `tanAcc` is null. MikkTSpace operates on an accessor-style interface —
provide callbacks that read from `std::vector<TempVertex>` and the index buffer.

---

### 3.3 No skinning weight normalization

**File:** `MeshProcessor.cpp`, vertex loop

glTF requires joint weights to sum to 1.0, but many exporters (Blender, Maya, and others
under certain conditions) produce slightly denormalized weights due to floating-point
quantization in the export. No normalization pass means subtly incorrect skinning on real
character content — usually visible as slight volume loss or surface swimming.

**Fix:** after reading weights, normalize before writing:

```cpp
float wsum = dst.jointWeights[0] + dst.jointWeights[1]
           + dst.jointWeights[2] + dst.jointWeights[3];
if (wsum > 1e-6f)
{
    const float inv = 1.f / wsum;
    dst.jointWeights[0] *= inv; dst.jointWeights[1] *= inv;
    dst.jointWeights[2] *= inv; dst.jointWeights[3] *= inv;
}
```

---

### 3.4 Always emits 32-bit indices

**File:** `MeshProcessor.cpp`, `combinedIndices` / `BinaryFormats.hpp`

`combinedIndices` is always `std::vector<uint32_t>`. The vast majority of props,
characters, and environment pieces have fewer than 65,535 vertices and could use
16-bit indices, halving index buffer size and improving GPU index cache efficiency.

**Fix:**
- Add an `indexType` field (1 byte) to `MeshHeaderDisk` to flag `uint16` vs `uint32`.
- After combining all primitives, check `combinedVerts.size()`. If ≤ 65535, write
  `uint16_t` indices; otherwise write `uint32_t`.
- Update the runtime loader to handle both.

---

### 3.5 No vertex deduplication / welding

**File:** `MeshProcessor.cpp`

When multiple glTF primitives share vertices (or when the exporter produces duplicates
at UV seams), the current pipeline writes every vertex as-is. No spatial welding is
performed. In practice this means meshes from tools like Houdini or Maya can arrive
with 15–30% redundant vertices.

For a full AAA pipeline, a post-process step using a hash map keyed on
`(position, normal, uv, ...)` to deduplicate and rebuild the index buffer is expected.
The precision threshold should be configurable (strict equality is usually fine for
UV-seam vertices which must be distinct).

---

### 3.6 No binary material format — TOML passes through raw

**File:** `PakWriter.cpp`, `ProcessAsset` — `.toml` / `.material`

`MaterialImporter` generates `properties.toml` files. `PakWriter` then packs them as
raw text. The runtime engine therefore has to:
1. Include a TOML parser.
2. Parse and validate text at load time on every material load.
3. Handle missing keys, type mismatches, and malformed files at runtime.

A proper pipeline converts `.properties.toml` → binary `.material` at pack time, using
`MaterialHeaderDisk` (which already exists in `BinaryFormats.hpp`). The TOML is then
an authoring artifact, not a shipping artifact.

**Fix:** add a `MaterialProcessor` that:
1. Parses the TOML (a lightweight parser like `toml++` is single-header).
2. Fills a `MaterialHeaderDisk`.
3. Appends the texture path table.
4. Returns binary bytes.

Dispatch `.toml` files whose parent path contains `.material` to this processor in
`ProcessAsset`. The engine then gets a fixed-layout binary blob it can `memcpy` into
structs.

---

### 3.7 `.webp` not dispatched to TextureProcessor

**File:** `PakWriter.cpp`, `ProcessAsset`

`.webp` is in `IsAlreadyCompressed`'s skip list (no zstd re-compression) but is absent
from the `ProcessAsset` dispatch table. So `.webp` files pack as raw WebP. Unless the
engine has a WebP decoder and uses it for all texture loads, these textures will fail
to load at runtime.

If the engine expects `.texture` (DDS/BCn), add `.webp` to the `ProcessAsset` dispatch:
```cpp
if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
    ext == ".tga" || ext == ".bmp" || ext == ".webp")
```

stb_image already supports WebP. Also remove `.webp` from `IsAlreadyCompressed` since
after transcoding to DDS the compressed block data should still go through zstd.

---

### 3.8 Sphere bound uses non-tight AABB centroid

**File:** `MeshProcessor.cpp`, `ComputeBounds()`

The sphere is centred on the AABB midpoint, then the radius is set to the maximum
distance from any vertex to that centre. The AABB midpoint is not the optimal
miniball centre — for elongated or asymmetric meshes (a sword, a stretched terrain
tile) the sphere can be 20–40% larger than necessary. Larger bounding spheres cause
false positive culling hits and hurt draw call rejection rates.

**Fix:** replace with Ritter's algorithm (two passes, simple to implement, not
optimal but much better than AABB midpoint):

```cpp
// Pass 1: find approximate diameter pair via 6 extreme points
// Pass 2: expand sphere to include any point outside it
// This gives a sphere within ~5% of optimal for typical mesh shapes
```

Or use the Welzl miniball for exact results (recursive, but mesh vertex counts are
bounded in a packer context).

---

### 3.9 Only one joint/weight set (max 4 influences)

**File:** `MeshProcessor.cpp`

The code reads `JOINTS_0` / `WEIGHTS_0` only. glTF supports up to 8 influences via
`JOINTS_1` / `WEIGHTS_1`. High-fidelity character meshes (especially around
shoulders and wrists) are commonly authored with 6–8 influences in DCC tools.
Silently dropping the second set produces visible skinning artefacts on these meshes
with no error or warning.

**Fix:** at minimum, log a warning when `JOINTS_1` is present and being ignored.
Ideally, read both sets and renormalize the combined 8-influence weight set before
selecting the top 4 by weight and renormalizing again. The `DiskMeshVertex` format
would need extending to support 8 influences for full support.

---

### 3.10 Flat normal accumulation — no angle-weighted averaging

**File:** `MeshProcessor.cpp`, `GenerateNormals()`

The fallback normal generator adds the raw cross product (face area proportional) to
each vertex, then normalizes. This gives area-weighted smooth normals, which is better
than flat shading but still incorrect around hard edges and for very non-uniform
triangle distributions. Industry standard is angle-weighted normals (weight each face
contribution by the angle subtended at the vertex for that triangle), which gives
perceptually correct smooth shading on low-poly meshes.

```cpp
// Per-face contribution weight = angle at this vertex, not 1.0
// Compute using dot product of the two edge vectors at the vertex
const float cosAngle  = dot(normalize(e1), normalize(e2));
const float angleWeight = std::acos(std::clamp(cosAngle, -1.f, 1.f));
verts[j].normal += faceNormal * angleWeight;
```

---

### 3.11 BC7 encoding quality not configurable per asset type

**File:** `TextureProcessor.cpp`

`bc7enc_compress_block_params_init` uses the default (medium) quality preset for every
texture. In practice you want:
- **Fast / iteration build:** lowest quality, sub-second per texture.
- **Shipping build:** highest quality (`bc7enc_compress_block_params_init_slow`), especially
  for albedo and normal maps.
- **Per-type overrides:** roughness / AO / metallic maps at a lower quality level than
  albedo since spatial frequency matters more than colour accuracy.

The `--compress-level` flag already gates zstd quality — a `--texture-quality` flag
(or `fast`/`normal`/`best` enum passed through to `ToDDS`) would complete the picture.

---

## 4. Polish / Minor

### 4.1 `AppendStr` silently truncates strings > 65535 bytes

**File:** `MeshProcessor.cpp`

```cpp
void AppendStr(std::vector<std::byte>& buf, const std::string& s)
{
    const uint16_t len = static_cast<uint16_t>(s.size()); // silent truncation
```

A bone name or path longer than 65535 characters (pathological but possible from a
script-generated asset) silently truncates. Add an assert or explicit error:

```cpp
assert(s.size() <= std::numeric_limits<uint16_t>::max() && "string too long for uint16_t length prefix");
```

---

### 4.2 `IsImageExtension` uses `std::set` for a fixed tiny list

**File:** `MaterialImporter.cpp`

```cpp
static const std::set<std::string> kImageExts = { ".png", ".jpg", ... };
```

`std::set` involves heap allocation and a tree comparison per call. For a fixed 8-element
set of short strings, a sorted `std::array<std::string_view, N>` with `std::binary_search`
or a plain linear scan is both faster and avoids the allocation:

```cpp
static constexpr std::string_view kImageExts[] = {
    ".bmp", ".dds", ".jpeg", ".jpg", ".ktx2", ".png", ".tga", ".webp"
};
return std::binary_search(std::begin(kImageExts), std::end(kImageExts), ext);
```

---

### 4.3 `Stem()` helper duplicated across translation units

`Stem(const fs::path&)` is defined identically in the anonymous namespace of both
`MeshProcessor.cpp` and `PakWriter.cpp`. Move it to a small shared `PipelineUtils.hpp`
internal header, or remove the one in `MeshProcessor` and use `sourcePath.stem().string()`
directly (it's only called in two places there).

---

### 4.4 xxhash include strategy inconsistent across TUs

`MeshProcessor.cpp` uses `XXH_STATIC_LINKING_ONLY` + `XXH_IMPLEMENTATION`.
`PakWriter.cpp` uses `XXH_INLINE_ALL`.

These don't conflict under the current build but the intent is unclear. Pick one strategy
and document it. The cleanest approach for a build tool is a dedicated `ThirdPartyImpl.cpp`
that owns all single-header library implementations (`XXH_IMPLEMENTATION`,
`CGLTF_IMPLEMENTATION`, `STB_IMAGE_IMPLEMENTATION`) and uses `XXH_INLINE_ALL` only in
the one TU that needs the fastest possible hashing (PakWriter).

---

### 4.5 `animset` animation paths assume a fixed `animations/` virtual directory

**File:** `MeshProcessor.cpp`, line 697

```cpp
animPaths.push_back("animations/" + fileName);
```

And in `PakWriter.cpp`:
```cpp
outFiles.push_back({ "animations/" + fileName, std::move(animData) });
```

Both hardcode `animations/` as the output subdirectory regardless of where the source
`.gltf` lives. A source file at `characters/hero/hero.gltf` would produce animations
at `animations/hero_Idle.anim`, not `characters/hero/animations/hero_Idle.anim`. The
path in the `.animset` will match the entry in the pak, so it won't fail to load, but
it creates a flat global `animations/` namespace where name collisions between different
characters become likely. Derive the output directory from `virtualPath` instead.

---

### 4.6 CMakeLists — no explicit warning flags

**File:** `CMakeLists.txt`

The build has no `/W4` (MSVC) or `-Wall -Wextra -Wpedantic` (GCC/Clang) flags. The
data race in §1.2 and the truncation in §4.1 would both be caught by `-Wconversion` +
`-Wthread-safety-analysis` (Clang) at compile time.

```cmake
target_compile_options(AssetPacker PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wconversion -Werror>
)
```

---

## Summary Table

| # | File | Severity | Description |
|---|------|----------|-------------|
| 1.1 | MeshProcessor.cpp | **Critical** | Bounds computed on zeroed vertex data |
| 1.2 | TextureProcessor.cpp | **Critical** | Data race in `InitEncoders()` |
| 1.3 | MeshProcessor.cpp | **Critical** | Node world transforms not applied |
| 1.4 | MeshProcessor.cpp | **Critical** | Non-bone anim channels write `0xFFFFFFFF` node index |
| 2.1 | PakWriter.cpp | Correctness | Extra files (skel/anim) missing from manifest |
| 2.2 | PakWriter.cpp | Correctness | Extra file insertion order is non-deterministic |
| 2.3 | PakWriter.cpp | Correctness | Partial pak left on disk after error |
| 2.4 | MeshProcessor.cpp | Correctness | `cgltf_load_buffers` return value ignored |
| 2.5 | MeshProcessor.cpp | Correctness | `ComputeSkeletonHash` called 3× redundantly |
| 2.6 | PakWriter.cpp | Correctness | Duplicate struct definition / comment block |
| 2.7 | TextureProcessor.hpp | Correctness | Header says BC5, code uses BC7 |
| 3.1 | TextureProcessor.cpp | Production | No mip-map generation |
| 3.2 | MeshProcessor.cpp | Production | No tangent generation (MikkTSpace) |
| 3.3 | MeshProcessor.cpp | Production | No skinning weight normalization |
| 3.4 | MeshProcessor.cpp | Production | Always 32-bit indices |
| 3.5 | MeshProcessor.cpp | Production | No vertex deduplication |
| 3.6 | PakWriter.cpp | Production | No binary material converter — TOML ships raw |
| 3.7 | PakWriter.cpp | Production | `.webp` not dispatched to TextureProcessor |
| 3.8 | MeshProcessor.cpp | Production | Sphere bound non-tight (AABB centroid) |
| 3.9 | MeshProcessor.cpp | Production | Only 4 bone influences, no JOINTS_1 |
| 3.10 | MeshProcessor.cpp | Production | Normal gen uses area-weighting not angle-weighting |
| 3.11 | TextureProcessor.cpp | Production | BC7 quality not configurable |
| 4.1 | MeshProcessor.cpp | Polish | `AppendStr` silently truncates at uint16 max |
| 4.2 | MaterialImporter.cpp | Polish | `IsImageExtension` uses heap-allocated `std::set` |
| 4.3 | Both .cpp files | Polish | `Stem()` duplicated |
| 4.4 | Both .cpp files | Polish | xxhash include strategy inconsistent |
| 4.5 | MeshProcessor.cpp | Polish | Anim paths hardcoded to `animations/` root |
| 4.6 | CMakeLists.txt | Polish | No compiler warning flags |

---

## Recommended Fix Order

**Phase 1 — Fix before using on real content**
1.1 → 1.2 → 1.3 → 1.4 → 2.4 → 2.3 → 2.5

**Phase 2 — Fix before sharing/distributing builds**
2.1 → 2.2 → 3.3 → 3.7 → 2.7 → 4.1

**Phase 3 — Required for production shipping content**
3.1 (mips) → 3.2 (tangents) → 3.6 (binary materials) → 3.4 (16-bit indices) → 3.9 (8 influences)

**Phase 4 — Quality of life and performance**
3.5 → 3.8 → 3.10 → 3.11 → 4.x
