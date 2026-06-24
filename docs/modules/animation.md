# `animation/` - Skeletal animation, GPU skinning

The animation system is split into three GPU-side systems (blend, IK, root motion) plus a CPU-side compiler and runtime database. All three GPU systems share the same per-frame pattern and are wired into `RenderQueue`.

## Files

| File | Role |
|---|---|
| `AnimationSystem.hpp` / `AnimationSystem.cpp` | Base class for animation systems. |
| `AnimationBlend.hpp` / `AnimationBlend.cpp` | Clip blending, weight normalization. |
| `AnimationRootMotion.hpp` / `AnimationRootMotion.cpp` | Root motion extraction. |
| `AnimationCompiler.hpp` / `AnimationCompiler.cpp` | Converts source animation data to runtime format. |
| `AnimationDatabase.hpp` / `AnimationDatabase.cpp` | Runtime storage for animation clips. |

## GPU animation systems

`src/engine/animation/AnimationBlend.hpp` and `AnimationRootMotion.hpp`. Each follows the same pattern:

```cpp
class AnimationBlendSystem {
public:
    void Init(VmaAllocator allocator, VkDevice device, uint32_t maxEntities, uint32_t maxClips);
    void Shutdown(VkDevice device);
    // per-frame: ...
};
```

- Initialized in `AetherCore` constructor (`src/engine/AetherCore.cpp:153`).
- Registered on the service container.
- Wired into `RenderQueue` so the forward pass can read the resulting skinning matrices via BDA.

`AnimationRootMotion` produces a signal semaphore + value that the graphics queue waits on before consuming its output.

## `RenderQueue` integration

`AetherCore.cpp:161` wires the systems into the queue:

```cpp
RenderQueue& rq = m_rendering->GetRenderQueue();
rq.SetAnimationBlendSystem(m_animationBlend.get());
rq.SetRootMotionSystem(m_rootMotion.get());
rq.SetHipsNodeIndex(0);
```

When a draw is skinned, the forward pass reads skinning matrices directly from the BDA of the corresponding animation system.

## `AnimationCompiler`

`src/engine/animation/AnimationCompiler.hpp`. Converts source animation data (the AetherCore intermediate format produced by the glTF loader or the asset packer) into the runtime representation. Uses the worker pool from `AssetSubsystem::GetUploadPool`.

`SetAnimationCompilePool` is called from `AetherCore.cpp:93` to install the pool.

## `AnimationDatabase`

`src/engine/animation/AnimationDatabase.hpp`. Runtime storage for animation clips. Backed by `GpuHeap` for the heavy data and uses a small CPU-side metadata index.

## Adding a new animation feature

1. Add a new system class mirroring the existing `AnimationBlendSystem` / `AnimationRootMotionSystem` pattern.
2. Init in `AetherCore` (after step 9 in the subsystem init order), allocate from `GpuHeap`.
3. Register on the service container.
4. If the system produces data the forward pass needs, add a BDA field to the relevant pass and an accessor to the system.
5. Add a setter on `RenderQueue` and wire it in `AetherCore.cpp`.

## See also

- [`modules/assets.md`](assets.md) - glTF loading and clip source data.
- [`modules/rendering.md`](rendering.md) - `RenderQueue` and `ForwardPass`.
- [`modules/vulkan.md`](vulkan.md) - `GpuHeap` for storage.
