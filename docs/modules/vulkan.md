# `vulkan/` - Vulkan implementation

The `vulkan/` module is the **only** place in the engine that includes `<vulkan/*>`. It implements the engine-facing API declared in `gpu/`.

## Files

| File | Role |
|---|---|
| `VulkanContext.hpp` / `VulkanContext.cpp` | Owns `VkInstance`, `VkDevice`, `VkPhysicalDevice`, VMA, queue handles. |
| `GraphicsDevice.hpp` / `GraphicsDevice.cpp` | Queue selection, `volk` loading, vk-bootstrap device creation. |
| `Swapchain.hpp` / `Swapchain.cpp` | Swapchain image management. |
| `UniqueBuffer.hpp` / `UniqueBuffer.cpp` | Move-only RAII `VkBuffer` wrapper. |
| `UniqueImage.hpp` / `UniqueImage.cpp` | Move-only RAII `VkImage` + `VkImageView` wrapper. |
| `GpuHeap.hpp` / `GpuHeap.cpp` | Device-local memory arena. |
| `GpuSpan.hpp` | Typed view over `GpuHeap` allocation with BDA. |
| `ResourcePool.hpp` / `ResourcePool.cpp` | Render-graph virtual resource aliasing pool. |
| `ShaderUtils.hpp` / `ShaderUtils.cpp` | SPIR-V / Slang helpers. |
| `AftermathContext.hpp` / `AftermathContext.cpp` | Optional NVIDIA Nsight Aftermath crash dumps. |
| `VmaImplementation.cpp` | VMA function pointer setup. |
| `VolkImplementation.cpp` | `volk` loader. |

## `VulkanContext`

`src/engine/vulkan/VulkanContext.hpp`. Owns the long-lived Vulkan objects:

- `VkInstance`
- `VkPhysicalDevice`
- `VkDevice`
- `VmaAllocator` (the global GPU memory allocator)
- Queue family indices and `VkQueue` handles

It exposes accessors used by the engine layer (`GpuDevice::GetVulkanContext().GetDevice().device`, etc.).

`GpuDevice::Shutdown` destroys VMA last to honor the rule "VMA must outlive all allocations it manages".

## `GraphicsDevice`

`src/engine/vulkan/GraphicsDevice.hpp`. Wraps `vk-bootstrap` for instance/device creation and `volk` for function loading. Lives behind `GpuDevice::m_gfx`.

## `Swapchain`

`src/engine/vulkan/Swapchain.hpp`. Owns the swapchain `VkSwapchainKHR`, per-image `VkImageView`s, and tracks image availability. Recreated on window resize by `GpuDevice::RecreateSwapchain`.

## `UniqueBuffer` / `UniqueImage`

Move-only RAII wrappers. Both follow the same pattern:

```cpp
class UniqueBuffer {
public:
    static Expected<UniqueBuffer> Create(const BufferDesc& desc);
    UniqueBuffer(const UniqueBuffer&) = delete;
    UniqueBuffer(UniqueBuffer&&) noexcept;
    UniqueBuffer& operator=(UniqueBuffer&&) noexcept;
    ~UniqueBuffer();

    [[nodiscard]] VkBuffer Get() const;
    [[nodiscard]] const VmaAllocation& GetAllocation() const;
    // ...
};
```

- Static `Create` factory returns `Expected<UniqueBuffer>`.
- Copy deleted, move defined, destructor releases the VMA allocation.
- All `GpuDevice` lifecycle rules (VMA outlives allocations, `WaitIdle` before destruction) apply.

If you need GPU memory outside the heap, use `UniqueBuffer` or `UniqueImage`. For high-frequency small allocations, use `GpuHeap`.

## `GpuHeap` / `GpuSpan<T>`

A device-local arena. The heap:

- Holds a sorted free-list of variable-size blocks.
- `Alloc<T>(n)` returns a `GpuSpan<T>` with a stable buffer device address (BDA).
- `Free(span)` returns the block to the free-list.

`GpuSpan<T>` is a typed view:

```cpp
template<typename T>
struct GpuSpan {
    VkBuffer        buffer;
    VmaAllocation   allocation;
    uint64_t        deviceAddress; // BDA
    T*              mapped;        // null if not host-visible
    size_t          count;
    // ...
};
```

`MeshArena` (in `AssetSubsystem`) and `AnimationDatabase` use the heap for their storage.

**Thread safety:** `GpuHeap` is **not** thread-safe. Only call from the asset loading thread.

## `ResourcePool`

`src/engine/vulkan/ResourcePool.hpp`. The render graph's virtual resource aliasing pool. The render graph can declare transient resources that get backed by a smaller pool of physical resources, reused across frames. Decouples pass resource declarations from physical allocation.

## `ShaderUtils`

Wraps SPIR-V loading, Slang compilation, and module/entry-point creation. The Slang integration is opt-in via `AETHERCORE_ENABLE_SLANG` (default ON).

## Synchronization

- `volk` for Vulkan function loading (`VK_NO_PROTOTYPES` defined).
- `vk-bootstrap` for instance/device creation.
- VMA for GPU memory (`VMA_DYNAMIC_VULKAN_FUNCTIONS=1`).
- All barriers use `vkCmdPipelineBarrier2` (synchronization2).

## Threading

- Vulkan-object creation happens on the engine thread during `GpuDevice::Init`.
- Swapchain recreation happens on the engine thread (`GpuDevice::RecreateSwapchain`).
- Per-frame command recording, submission, and presentation happen on the render thread.

## See also

- [`modules/gpu.md`](gpu.md) - engine-facing API.
- [`modules/rendering.md`](rendering.md) - how the renderer uses these.
