# `material/` - PBR materials, bindless descriptors, textures

`material/` defines the PBR material model and its GPU representation. Materials live in a bindless storage buffer addressed by index from the `ForwardPass`.

## Files

| File | Role |
|---|---|
| `Material.hpp` | The PBR material struct (CPU side). |
| `GpuMaterial.hpp` | The GPU-side PBR material layout. |
| `MaterialBuffer.hpp` / `MaterialBuffer.cpp` | The host of all materials; manages the bindless storage buffer. |
| `BindlessContract.hpp` / `BindlessContract.cpp` | Bindless layout contracts shared by shaders and CPU. |
| `Texture.hpp` / `Texture.cpp` | Texture handle + bindless sampled-image index. |

## PBR model

AetherCore uses a metallic-roughness PBR model:

| Channel | Source |
|---|---|
| Base color | `albedo` (RGB) + `baseColorFactor` (RGBA) |
| Metallic | `metallic` (R) + `metallicFactor` |
| Roughness | `roughness` (R) + `roughnessFactor` |
| Normal | `normal` (RGB) - tangent-space |
| Occlusion | `occlusion` (R) |
| Emissive | `emissive` (RGB) + `emissiveFactor` |

## `MaterialBuffer`

`src/engine/material/MaterialBuffer.hpp`. Owns:

- A CPU-side `std::vector<Material>` indexed by `materialIndex`.
- A bindless storage buffer that mirrors the active materials.

Material updates use a staging pattern: CPU writes to the staging copy, and at the right frame boundary the data is uploaded to the bindless storage buffer. The GPU address of the storage buffer is snapshotted into `RenderFramePacket::materialBufferAddr` and read by the forward pass.

## `BindlessContract`

`src/engine/material/BindlessContract.hpp`. A shared header that defines the bindless descriptor set layout and the per-binding contracts (counts, types, slot meaning). Both the C++ side (`BindlessManager`) and the Slang shaders `#include` (or otherwise reference) this contract.

## `Texture`

`src/engine/material/Texture.hpp`. A handle into the bindless sampled-image array. The actual `VkImage` lives in `vulkan/`; the engine-facing handle is just an index plus metadata (format, extent, mip count).

## See also

- [`modules/gpu.md`](gpu.md) - bindless manager.
- [`modules/assets.md`](assets.md) - material loading.
- [`modules/passes.md`](passes.md) - how the forward pass reads materials.
