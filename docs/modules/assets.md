# `assets/` - Asset subsystem, glTF loading, mesh and material staging

`assets/` is responsible for loading, processing, and staging game assets on the GPU. It builds on the virtual file system in `io/` and the GPU heap in `vulkan/`.

## Files

| File | Role |
|---|---|
| `AssetSubsystem.hpp` / `AssetSubsystem.cpp` | Subsystem façade. Owns `AssetManager`, `MeshArena`, `MaterialBuffer`, `MeshUploadQueue`. Registers them on the service container. |
| `AssetManager.hpp` / `AssetManager.cpp` | High-level asset handle table. Maps `AssetId` to loaded data. |
| `GltfAsset.hpp` / `GltfAsset.cpp` | glTF 2.0 loader (via cgltf 1.15). |

## `AssetSubsystem`

`src/engine/assets/AssetSubsystem.hpp`. The subsystem façade. Lifecycle:

- `Init(ServiceContainer&)` - builds `AssetManager`, `MeshArena`, `MaterialBuffer`, `MeshUploadQueue`, and an upload thread pool.
- `Shutdown()` - flushes pending uploads, frees device-local allocations.
- `LinkRenderingDeps(ServiceContainer&)` - wires the material buffer address into the render queue.

Accessors registered on the service container:

- `AssetManager` - handle table.
- `MeshArena` - device-local mesh storage.
- `MeshUploadQueue` - staging pipeline.
- `MaterialBuffer` - PBR material storage.

## `AssetManager`

`src/engine/assets/AssetManager.hpp`. Maps `AssetId` (xxHash-based) to a typed asset handle. Typical usage:

```cpp
auto modelHandle = assets.LoadModel("assets://models/tree.glb");
auto material    = assets.GetMaterial(materialHandle);
auto mesh        = assets.GetMesh(modelHandle, 0);
```

The `AssetId` is content-hashed, so re-loading the same file reuses the existing handle. Asset handles are stable across frames.

## `MeshArena`

Device-local mesh storage backed by `GpuHeap`. Holds:

- Vertex buffers.
- Index buffers.
- Mesh metadata (bounds, generation counter).

Each mesh in the arena is identified by `(slot, generation)`. The generation counter allows the same slot to be re-used after the previous owner has been retired.

## `MeshUploadQueue`

Staging pipeline that pushes arena data into the GPU heap at the right frame boundary. The queue is filled on the IO thread and drained on the engine thread (or render thread) at the chosen frame stage.

## `MaterialBuffer`

`MaterialBuffer` (in `material/MaterialBuffer.hpp`) holds PBR material data. Each material has:

- Base color factor, metallic, roughness, emissive.
- Bindless texture indices for albedo, normal, metallic-roughness, occlusion, emissive.

The buffer is uploaded to a bindless storage buffer that shaders read by index.

## `GltfAsset`

`src/engine/assets/GltfAsset.hpp`. Loader for glTF 2.0 files via cgltf 1.15. Supports:

- Static meshes.
- Materials (PBR metallic-roughness).
- Textures (with KTX2 / PNG / JPG source formats).
- Skins (joint indices and weights).
- Animations (clips, channels, samplers).

Loaded glTF data is converted into AetherCore's runtime representation and stored in `MeshArena` + `AnimationDatabase`.

## LoadingManager integration

`src/engine/utils/LoadingManager.hpp` tracks async load progress. `LoadingLayer` renders a full-screen overlay with a progress bar. Together with the IO thread's coroutines, this gives the user feedback during heavy asset loads.

## Threading

| Operation | Thread |
|---|---|
| `AssetManager::LoadModel` | IO thread (coroutine) |
| `MeshUploadQueue` production | IO thread |
| `MeshUploadQueue` drain | engine thread, at frame boundary |
| `GpuHeap` access | IO thread only (not thread-safe) |
| `MaterialBuffer` updates | engine thread (or IO, with external sync) |

## See also

- [`modules/io.md`](io.md) - virtual file system and async I/O.
- [`modules/material.md`](material.md) - material types.
- [`modules/animation.md`](animation.md) - animation database.
- [`modules/vulkan.md`](vulkan.md) - `GpuHeap` reference.
