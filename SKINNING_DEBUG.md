# Skinning Geometry Explosion - Debug Tracker

## Confirmed Symptoms
- Geometry explosion ONLY on skinned meshes (Fox model)
- Non-skinned meshes (cubes, spheres) render correctly
- Issue appeared when GPU skinning compute shaders were introduced
- Visible in RenderDoc on the indirect sub-command
- **y-axis is plausible (-2.77 to +0.36), x/z are HUGE (-11534 to +30882, -19305 to +7209)**

## Hypotheses Tried

### 1. SV_InstanceID double-counting [APPLIED, DID NOT FIX]
- **What**: `SV_InstanceID` in Slang maps to `gl_InstanceIndex` (base-inclusive).
  Adding `startInstance + instanceId` double-counts `firstInstance`.
- **Fix applied**: Removed `instanceId : SV_InstanceID` from all 4 vertex shaders
  (gltf_mesh.slang, shadow_depth.slang, voxel_shadow_depth.slang, voxel_chunk.slang).
  Now uses only `startInstance : SV_StartInstanceLocation`.
- **Result**: Geometry still exploded.

### 2. Shaders not recompiled after fix [RULED OUT]
- **What**: Checked .spv timestamps vs .slang source timestamps.
- **Result**: Shaders are up-to-date. Build is not the issue.

### 3. Struct layout mismatch [RULED OUT]
- **What**: Verified all C++ structs have static_assert size/offset checks matching shader layouts.
- DrawInstanceData (104 bytes), DrawPushConstants (24 bytes), SkinCopyJob (32 bytes),
  SkinPalettePush (64 bytes), SampledNodePose (48 bytes), AeBnVertex (92 bytes) all verified.

### 4. QuatToMat / ComposeTrs math [INVESTIGATED, STILL BUGGY]
- **What**: Tried both column-major and row-major conventions in `skin_palette_build.slang`.
- **Current state**: Both `QuatToMat` and `ComposeTrs` have been rewritten multiple times.
  GPU debug output shows garbage matrices (4th row not [0,0,0,1], huge translations).
- **Key finding**: Slang/HLSL `float4x4(a,b,c,d)` sets COLUMNS, not rows.
  The `mul()` order (pre vs post multiply) also matters for hierarchy accumulation.
- **Result**: Still producing garbage matrices. The matrix math is the likely root cause.

### 5. Hierarchy walk termination [RULED OUT]
- **What**: Root nodes have `parentIndex = -1`. The `while (walk >= 0 ...)` loop exits correctly.

### 6. Skin palette address threading [RULED OUT]
- **What**: Verified both compute and vertex shader use the same `currSkinPaletteAddr`.
- `SkinCopyJob.dstPaletteOffset` matches `DrawInstanceData.skinPaletteOffset`.

### 7. Memory barriers missing [RULED OUT]
- **What**: Fill → animToSkin → skinToShaders barriers are all present and correctly scoped.

### 8. Animation sample addressing [RULED OUT]
- **What**: `sampled[nodeIndex]` matches where `animation_sample` writes `poses[poseBase + nodeIndex]`.

### 9. timesOffset/valuesOffset unit mismatch [RULED OUT]
- **What**: Both are BYTE offsets. Shader correctly divides by 4 and 16.

### 10. animPath enum values [RULED OUT]
- **What**: Translation=0, Rotation=1, Scale=2 match shader constants.

### 11. First-frame initialization race [RULED OUT]
- **What**: `vkCmdFillBuffer` clears skin palette to zero before compute.

### 12. Multi-queue conflict [RULED OUT]
- **What**: Each render queue has independent skin palette and instance data buffers.

### 13. nodeIndex space: global vs joint-list [VERIFIED CORRECT]
- **What**: `GpuChannel.nodeIndex` = global node index. `skinJoints[j]` = global node index.
- **Fox.gltf**: 1 skin, 24 joints (nodes 2-25), 26 nodes total.
- **Result**: Indexing is correct. skinJoints = [2,3,4,...,25].

### 14. CPU data validation [ALL CORRECT]
- **Bind poses**: Node 4 (b_Hip_01) T=[0, 26.75, 42.94] matches Fox.gltf ✓
- **Skin joints**: [2,3,4,...,25] - valid node indices ✓
- **Inverse bind matrices**: Plausible values (InvBind[2] has T=[-30.64,-40.26,0]) ✓
- **Result**: All CPU-side data is correct. Bug is in GPU shaders.

### 15. GPU skin matrix debug dump [ADDED, SHOWS BUG]
- **What**: Added `m_debugSkinMatrixBuffer` (256 bytes, host-visible) to dump first 4 skin matrices.
- **Shader**: `skin_palette_build.slang` writes to `pc.debugSkinMatricesAddr`.
- **C++**: Read back in `FlushDraw` via `vkDeviceWaitIdle`.
- **GPU output**: Matrices have garbage 4th row (not [0,0,0,1]) and huge translations.
- **Example**: SkinMat[2] 4th row = [-66.62, 85.94, -38.61, -4655.97] ← should be [0,0,0,1]
- **Conclusion**: `QuatToMat` and/or `ComposeTrs` are producing wrong matrices.

## What We Know
- Non-skinned geometry: correct ✓
- Skinning compute shaders DO dispatch ✓
- `skinPaletteAddr` is non-zero at draw time ✓
- Vertex shader IS taking the skinning path ✓
- **All CPU data is correct** (bind poses, skin joints, inverse binds) ✓
- **GPU matrices are garbage** (wrong rotation, wrong translation, wrong 4th row)
- **Root cause: matrix math in `skin_palette_build.slang` is wrong**

## Debug Infrastructure Added
- `AnimationDatabase.hpp/cpp`: CPU-side accessors for bind poses, skin joints, inverse binds
- `RenderQueue.cpp`: Debug log dump of bind poses, skin joints, inverse bind matrices
- `RenderQueue.hpp/cpp`: `m_debugSkinMatrixBuffer` for GPU→CPU matrix readback
- `GpuContracts.hpp`: `SkinPalettePush.debugSkinMatricesAddr` push constant field
- `skin_palette_build.slang`: Debug write of first 4 skin matrices to debug buffer

## FIXED (partial)

### Fix 1: `float4x4(1.0)` is all-ones, not identity [FIXED, CONFIRMED WORKING]
- `float4x4(scalar)` in Slang broadcasts to ALL 16 elements. `float4x4(1.0)` = 16 ones.
- **Fix**: explicit 4-column identity construction in `skin_palette_build.slang` line ~73.
- **Evidence**: SkinMat[0] (node 2, identity pose + identity invBind) now reads `[1,0,0,0] [0,1,0,0] [0,0,1,0] [0,0,0,1]` ✓

### Convention (confirmed, all correct, do not change)
- Slang → SPIR-V: `float4x4(v0,v1,v2,v3)` sets COLUMNS; `M[i]` = column i; `mul(M,v)` = M*v
- `QuatToMat`, `ComposeTrs`, hierarchy walk order, `mul(globalPose, invBind)`, vertex shader: all CORRECT

---

## Remaining Bug: InvBind matrices arrive TRANSPOSED on GPU

### Evidence
```
SkinMat[1] (node 3, bind R=R(-90°X), invBind should be R(+90°X)):
  Expected: identity
  Got:      [1,0,0,0] [0,-1,0,0] [0,0,-1,0] [0,0,0,1]  = R(-180°X)
```
`R(-90°X) * R(-90°X) = R(-180°X)` — the GPU is multiplying bind pose × bind pose instead of bind pose × inverse-bind.

SkinMat[2] (has translation) has nonzero 4th elements in columns 0-2 and w≠1 in col3 — classic sign that a transposed affine matrix (translation in row 3 instead of col 3) was multiplied in.

### Root cause location
`GltfAsset.cpp:203`: `std::memcpy(&skin.inverseBindMatrices[j][0][0], p, 64)`
- This raw-copies 64 bytes into `glm::mat4` (column-major).
- If the `.aebn` binary stores the matrix in **row-major** order, the memcpy silently loads the **transpose** of the intended matrix.
- CPU debug log shows the correct R(+90°X) because the print reads `m[col][row]` — but those same bytes, when interpreted as a column-major float4x4 on GPU, ARE R(+90°X)... however the GPU result is wrong.
- **TODO**: check the `.aebn` exporter to confirm whether it writes invBind data row-major or column-major.

### Candidate fixes (pick one after confirming exporter format)
A. If `.aebn` exporter writes row-major: transpose on load in `GltfAsset.cpp`:
   ```cpp
   glm::mat4 tmp;
   std::memcpy(&tmp[0][0], p, 64);
   skin.inverseBindMatrices[j] = glm::transpose(tmp);
   ```
B. If exporter is correct and the issue is the upload path: transpose in shader:
   ```slang
   float4x4 skinMatrix = mul(globalM, transpose(skinInverseBinds[invBindBase + j]));
   ```
C. Verify by dumping the raw 64 bytes of invBind[1] from the binary and checking whether [0,0,1,0] appears at bytes 16-19 (column-major) or 8-11 (row-major).


### Fix 2: InvBind matrices loaded from .aebn with transpose [APPLIED, DID NOT FIX]
- **What**: `GltfAsset.cpp:203` was doing raw memcpy from .aebn into `glm::mat4`.
  If .aebn stored row-major, this would load transposed matrices.
- **Fix applied**: `glm::mat4 tmp; std::memcpy(&tmp[0][0], p, 64); skin.inverseBindMatrices[j] = glm::transpose(tmp);`
- **Result**: 4th row became `[0,0,0,1]` (correct), but skin matrices still wrong.
  SkinMat[2] translation = `[7.11, -70.48, 0]` instead of near-identity.
- **Reverted**: The transpose was wrong. The .aebn stores invBind in column-major (glTF standard),
  so raw memcpy into glm::mat4 is correct. Reverted to original memcpy.

### Hypothesis 16: Bind pose TRS vs inverse bind matrix mismatch [INVESTIGATING]
- **What**: The inverse bind matrices in glTF/Fox.gltf were computed from the world transform
  of each joint at bind time. The shader computes world transforms by walking the hierarchy
  from local TRS. These should produce the same result, but they don't.
- **Evidence**: 
  - Node 4 (b_Hip_01) bind pose: T=[0, 26.75, 42.94], R=[0.128,-0.695,-0.128,0.695]
  - Hierarchy: 4 -> 3 -> 2 -> 0 (root)
  - Expected world transform of node 4 at bind time: includes R(-90°X) from node 3
  - InvBind[2] from CPU: translation column = [-30.64, -40.26, 0.00, 1.00]
  - Python computation of expected inverse world translation: [-30.64, -40.26, 0] ✓ MATCHES
  - **Conclusion**: Inverse bind matrices ARE correct. The bug is in the shader's globalM computation.
- **Next step**: Added debug output to dump intermediate `globalM` after each hierarchy step
  to see where the computation diverges from expected.

### Debug infrastructure update
- Increased debug buffer from 256 to 1024 bytes to hold skin matrices, sampled poses, and intermediate globalM
- Shader now dumps: (1) first 4 skin matrices at float offset 0, (2) sampled poses for nodes 0-5 at float offset 64, (3) intermediate globalM after each hierarchy step for joint 2 at float offset 140
- C++ reads back and prints all three sections
- **CRITICAL FINDING**: Sampled poses are CORRECT (Node 4: T=[0,24.55,41.08] R=[0.128,-0.695,-0.128,0.695]), but all intermediate globalM steps are zeros. This means `mul()` is producing zeros even with valid input data.
- **Hypothesis**: The `mul()` function in Slang/SPIR-V may be producing zeros due to a matrix layout mismatch, OR the `ComposeTrs` function is returning a zero matrix despite valid input.
- **Next step**: Simplified debug to isolate the bug:
  1. Dump `ComposeTrs(node4)` output directly (offset 0)
  2. Dump sampled pose for node 4 (offset 16)
  3. Dump identity matrix (offset 32)
  4. Dump `mul(ComposeTrs, identity)` result (offset 48)
  5. Dump first 4 skin matrices (offset 64)
  This will tell us exactly which step produces zeros.

## Most recent logs (simplified debug: ComposeTrs test)

```
RenderQueue SkinJob dump (frame 0, 1 jobs, 1 sampleJobs):
  dstPaletteAddr=0x37b600000
  nodeParentsAddr=0x378390ef0  skinMetasAddr=0x378391440  skinJointsAddr=0x378391450  skinInverseBindsAddr=0x3783914b0
  Job[0]: dstOff=0 joints=24 skin=0 nodes=26 sampledAddr=0x37f800000
  SampleJob[0]: clip=0 time=0.003 poseOff=0 nodeCount=26
  === BIND POSE DUMP (nodeCount=26) ===
    Node[0]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[1]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[2]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=0
    Node[3]: T=[0.00,0.00,0.00] R=[-0.707,0.000,0.000,0.707] S=[1.00,1.00,1.00] parent=2
    Node[4]: T=[0.00,26.75,42.94] R=[0.128,-0.695,-0.128,0.695] S=[1.00,1.00,1.00] parent=3
    Node[5]: T=[12.85,0.00,0.00] R=[0.000,0.000,-0.590,0.807] S=[1.00,1.00,1.00] parent=4
    Node[6]: T=[21.66,-0.00,0.00] R=[0.000,0.000,0.017,1.000] S=[1.00,1.00,1.00] parent=5
    Node[7]: T=[25.65,0.00,0.00] R=[0.000,0.000,0.303,0.953] S=[1.00,1.00,1.00] parent=6
    Node[8]: T=[13.38,0.00,0.00] R=[0.000,0.000,-0.400,0.916] S=[1.00,1.00,1.00] parent=7
    Node[9]: T=[18.68,-4.30,6.97] R=[0.000,-0.000,-0.712,0.702] S=[1.00,1.00,1.00] parent=6
  === SKIN JOINTS (first skin, 24 joints) ===
    Joint[0]: nodeIndex=2
    Joint[1]: nodeIndex=3
    Joint[2]: nodeIndex=4
    Joint[3]: nodeIndex=5
    Joint[4]: nodeIndex=6
    Joint[5]: nodeIndex=7
    Joint[6]: nodeIndex=8
    Joint[7]: nodeIndex=9
    Joint[8]: nodeIndex=10
    Joint[9]: nodeIndex=11
  === INVERSE BIND MATRICES (first 3) ===
    InvBind[0]: [1.00,-0.00,0.00,-0.00] [-0.00,1.00,-0.00,0.00] [0.00,-0.00,1.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[1]: [1.00,-0.00,0.00,-0.00] [-0.00,-0.00,1.00,0.00] [0.00,-1.00,-0.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[2]: [-0.00,-0.00,-1.00,0.00] [0.93,0.36,-0.00,0.00] [0.36,-0.93,0.00,-0.00] [-30.64,-40.26,0.00,1.00]
  ComposeTrs(node4)    [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  invBind[2] (GPU)     [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 0       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 1       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 2       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 3       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  skinMatrix (joint2)  [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
RenderQueue SkinJob dump (frame 0, 1 jobs, 1 sampleJobs):
  dstPaletteAddr=0x37ce00000
  nodeParentsAddr=0x378390ef0  skinMetasAddr=0x378391440  skinJointsAddr=0x378391450  skinInverseBindsAddr=0x3783914b0
  Job[0]: dstOff=0 joints=24 skin=0 nodes=26 sampledAddr=0x381c00000
  SampleJob[0]: clip=0 time=0.003 poseOff=0 nodeCount=26
  === BIND POSE DUMP (nodeCount=26) ===
    Node[0]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[1]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[2]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=0
    Node[3]: T=[0.00,0.00,0.00] R=[-0.707,0.000,0.000,0.707] S=[1.00,1.00,1.00] parent=2
    Node[4]: T=[0.00,26.75,42.94] R=[0.128,-0.695,-0.128,0.695] S=[1.00,1.00,1.00] parent=3
    Node[5]: T=[12.85,0.00,0.00] R=[0.000,0.000,-0.590,0.807] S=[1.00,1.00,1.00] parent=4
    Node[6]: T=[21.66,-0.00,0.00] R=[0.000,0.000,0.017,1.000] S=[1.00,1.00,1.00] parent=5
    Node[7]: T=[25.65,0.00,0.00] R=[0.000,0.000,0.303,0.953] S=[1.00,1.00,1.00] parent=6
    Node[8]: T=[13.38,0.00,0.00] R=[0.000,0.000,-0.400,0.916] S=[1.00,1.00,1.00] parent=7
    Node[9]: T=[18.68,-4.30,6.97] R=[0.000,-0.000,-0.712,0.702] S=[1.00,1.00,1.00] parent=6
  === SKIN JOINTS (first skin, 24 joints) ===
    Joint[0]: nodeIndex=2
    Joint[1]: nodeIndex=3
    Joint[2]: nodeIndex=4
    Joint[3]: nodeIndex=5
    Joint[4]: nodeIndex=6
    Joint[5]: nodeIndex=7
    Joint[6]: nodeIndex=8
    Joint[7]: nodeIndex=9
    Joint[8]: nodeIndex=10
    Joint[9]: nodeIndex=11
  === INVERSE BIND MATRICES (first 3) ===
    InvBind[0]: [1.00,-0.00,0.00,-0.00] [-0.00,1.00,-0.00,0.00] [0.00,-0.00,1.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[1]: [1.00,-0.00,0.00,-0.00] [-0.00,-0.00,1.00,0.00] [0.00,-1.00,-0.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[2]: [-0.00,-0.00,-1.00,0.00] [0.93,0.36,-0.00,0.00] [0.36,-0.93,0.00,-0.00] [-30.64,-40.26,0.00,1.00]
  ComposeTrs(node4)    [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  invBind[2] (GPU)     [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 0       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 1       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 2       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 3       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  skinMatrix (joint2)  [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
RenderQueue SkinJob dump (frame 0, 1 jobs, 1 sampleJobs):
  dstPaletteAddr=0x384000000
  nodeParentsAddr=0x378390ef0  skinMetasAddr=0x378391440  skinJointsAddr=0x378391450  skinInverseBindsAddr=0x3783914b0
  Job[0]: dstOff=0 joints=24 skin=0 nodes=26 sampledAddr=0x38c200000
  SampleJob[0]: clip=0 time=0.003 poseOff=0 nodeCount=26
  === BIND POSE DUMP (nodeCount=26) ===
    Node[0]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[1]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[2]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=0
    Node[3]: T=[0.00,0.00,0.00] R=[-0.707,0.000,0.000,0.707] S=[1.00,1.00,1.00] parent=2
    Node[4]: T=[0.00,26.75,42.94] R=[0.128,-0.695,-0.128,0.695] S=[1.00,1.00,1.00] parent=3
    Node[5]: T=[12.85,0.00,0.00] R=[0.000,0.000,-0.590,0.807] S=[1.00,1.00,1.00] parent=4
    Node[6]: T=[21.66,-0.00,0.00] R=[0.000,0.000,0.017,1.000] S=[1.00,1.00,1.00] parent=5
    Node[7]: T=[25.65,0.00,0.00] R=[0.000,0.000,0.303,0.953] S=[1.00,1.00,1.00] parent=6
    Node[8]: T=[13.38,0.00,0.00] R=[0.000,0.000,-0.400,0.916] S=[1.00,1.00,1.00] parent=7
    Node[9]: T=[18.68,-4.30,6.97] R=[0.000,-0.000,-0.712,0.702] S=[1.00,1.00,1.00] parent=6
  === SKIN JOINTS (first skin, 24 joints) ===
    Joint[0]: nodeIndex=2
    Joint[1]: nodeIndex=3
    Joint[2]: nodeIndex=4
    Joint[3]: nodeIndex=5
    Joint[4]: nodeIndex=6
    Joint[5]: nodeIndex=7
    Joint[6]: nodeIndex=8
    Joint[7]: nodeIndex=9
    Joint[8]: nodeIndex=10
    Joint[9]: nodeIndex=11
  === INVERSE BIND MATRICES (first 3) ===
    InvBind[0]: [1.00,-0.00,0.00,-0.00] [-0.00,1.00,-0.00,0.00] [0.00,-0.00,1.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[1]: [1.00,-0.00,0.00,-0.00] [-0.00,-0.00,1.00,0.00] [0.00,-1.00,-0.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[2]: [-0.00,-0.00,-1.00,0.00] [0.93,0.36,-0.00,0.00] [0.36,-0.93,0.00,-0.00] [-30.64,-40.26,0.00,1.00]
  ComposeTrs(node4)    [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  invBind[2] (GPU)     [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 0       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 1       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 2       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 3       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  skinMatrix (joint2)  [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
RenderQueue SkinJob dump (frame 0, 1 jobs, 1 sampleJobs):
  dstPaletteAddr=0x37641c690
  nodeParentsAddr=0x378390ef0  skinMetasAddr=0x378391440  skinJointsAddr=0x378391450  skinInverseBindsAddr=0x3783914b0
  Job[0]: dstOff=0 joints=24 skin=0 nodes=26 sampledAddr=0x379200000
  SampleJob[0]: clip=0 time=0.003 poseOff=0 nodeCount=26
  === BIND POSE DUMP (nodeCount=26) ===
    Node[0]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[1]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=-1
    Node[2]: T=[0.00,0.00,0.00] R=[0.000,0.000,0.000,1.000] S=[1.00,1.00,1.00] parent=0
    Node[3]: T=[0.00,0.00,0.00] R=[-0.707,0.000,0.000,0.707] S=[1.00,1.00,1.00] parent=2
    Node[4]: T=[0.00,26.75,42.94] R=[0.128,-0.695,-0.128,0.695] S=[1.00,1.00,1.00] parent=3
    Node[5]: T=[12.85,0.00,0.00] R=[0.000,0.000,-0.590,0.807] S=[1.00,1.00,1.00] parent=4
    Node[6]: T=[21.66,-0.00,0.00] R=[0.000,0.000,0.017,1.000] S=[1.00,1.00,1.00] parent=5
    Node[7]: T=[25.65,0.00,0.00] R=[0.000,0.000,0.303,0.953] S=[1.00,1.00,1.00] parent=6
    Node[8]: T=[13.38,0.00,0.00] R=[0.000,0.000,-0.400,0.916] S=[1.00,1.00,1.00] parent=7
    Node[9]: T=[18.68,-4.30,6.97] R=[0.000,-0.000,-0.712,0.702] S=[1.00,1.00,1.00] parent=6
  === SKIN JOINTS (first skin, 24 joints) ===
    Joint[0]: nodeIndex=2
    Joint[1]: nodeIndex=3
    Joint[2]: nodeIndex=4
    Joint[3]: nodeIndex=5
    Joint[4]: nodeIndex=6
    Joint[5]: nodeIndex=7
    Joint[6]: nodeIndex=8
    Joint[7]: nodeIndex=9
    Joint[8]: nodeIndex=10
    Joint[9]: nodeIndex=11
  === INVERSE BIND MATRICES (first 3) ===
    InvBind[0]: [1.00,-0.00,0.00,-0.00] [-0.00,1.00,-0.00,0.00] [0.00,-0.00,1.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[1]: [1.00,-0.00,0.00,-0.00] [-0.00,-0.00,1.00,0.00] [0.00,-1.00,-0.00,-0.00] [-0.00,0.00,-0.00,1.00]
    InvBind[2]: [-0.00,-0.00,-1.00,0.00] [0.93,0.36,-0.00,0.00] [0.36,-0.93,0.00,-0.00] [-30.64,-40.26,0.00,1.00]
  ComposeTrs(node4)    [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  invBind[2] (GPU)     [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 0       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 1       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 2       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  globalM step 3       [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  skinMatrix (joint2)  [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00] [0.00,0.00,0.00,0.00]
  ComposeTrs(node4)    [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,0.00] [0.00,24.55,41.07,1.00]
  invBind[2] (GPU)     [-0.00,0.93,0.36,-30.64] [-0.00,0.36,-0.93,-40.26] [-1.00,-0.00,0.00,0.00] [0.00,0.00,-0.00,1.00]
  globalM step 0       [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,-0.00] [0.00,24.55,41.07,1.00]
  globalM step 1       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 2       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 3       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  skinMatrix (joint2)  [-0.93,-0.13,0.33,14.30] [0.00,0.93,0.36,-30.64] [-0.36,0.33,-0.87,-37.63] [-41.07,8.72,-22.95,-987.36]
  ComposeTrs(node4)    [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,0.00] [0.00,24.55,41.07,1.00]
  invBind[2] (GPU)     [-0.00,0.93,0.36,-30.64] [-0.00,0.36,-0.93,-40.26] [-1.00,-0.00,0.00,0.00] [0.00,0.00,-0.00,1.00]
  globalM step 0       [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,-0.00] [0.00,24.55,41.07,1.00]
  globalM step 1       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 2       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 3       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  skinMatrix (joint2)  [-0.93,-0.13,0.33,14.30] [0.00,0.93,0.36,-30.64] [-0.36,0.33,-0.87,-37.63] [-41.07,8.72,-22.95,-987.36]
  ComposeTrs(node4)    [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,0.00] [0.00,24.55,41.07,1.00]
  invBind[2] (GPU)     [-0.00,0.93,0.36,-30.64] [-0.00,0.36,-0.93,-40.26] [-1.00,-0.00,0.00,0.00] [0.00,0.00,-0.00,1.00]
  globalM step 0       [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,-0.00] [0.00,24.55,41.07,1.00]
  globalM step 1       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 2       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 3       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  skinMatrix (joint2)  [-0.93,-0.13,0.33,14.30] [0.00,0.93,0.36,-30.64] [-0.36,0.33,-0.87,-37.63] [-41.07,8.72,-22.95,-987.36]
  ComposeTrs(node4)    [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,0.00] [0.00,24.55,41.07,1.00]
  invBind[2] (GPU)     [-0.00,0.93,0.36,-30.64] [-0.00,0.36,-0.93,-40.26] [-1.00,-0.00,0.00,0.00] [0.00,0.00,-0.00,1.00]
  globalM step 0       [0.00,-0.36,0.93,0.00] [-0.00,0.93,0.36,0.00] [-1.00,-0.00,-0.00,-0.00] [0.00,24.55,41.07,1.00]
  globalM step 1       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 2       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  globalM step 3       [0.00,-0.36,0.93,0.00] [1.00,-0.00,-0.00,0.00] [0.00,0.93,0.36,0.00] [0.00,24.55,41.07,1.00]
  skinMatrix (joint2)  [-0.93,-0.13,0.33,14.30] [0.00,0.93,0.36,-30.64] [-0.36,0.33,-0.87,-37.63] [-41.07,8.72,-22.95,-987.36]
```