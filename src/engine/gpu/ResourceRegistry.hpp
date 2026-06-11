#pragma once

#include <cstdint>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::gpu
{
	struct BufferDesc
	{
		DeviceSize  size      = 0;
		BufferUsage usage     = BufferUsage::None;
		const char* debugName = nullptr;
	};

	struct MappedBufferDesc
	{
		DeviceSize          size         = 0;
		BufferUsage         usage        = BufferUsage::None;
		MappedMemoryUsage   memoryUsage  = MappedMemoryUsage::Auto;
		const char*         debugName    = nullptr;
	};

	struct MappedBufferView
	{
		void*         mappedPtr     = nullptr;
		DeviceAddress deviceAddress = 0;
		DeviceSize    size          = 0;
	};

	struct TextureDesc
	{
		Format      format    = Format::Undefined;
		Extent2D    extent    = {};
		ImageUsage  usage     = ImageUsage::None;
		ImageAspect aspect    = ImageAspect::Color;
		const char* debugName = nullptr;
	};

	struct ResourceRegistryInitDesc
	{
		void* vulkanDevice    = nullptr;
		void* vmaAllocator    = nullptr;
		void* backendRegistry = nullptr;
	};

	class ResourceRegistry
	{
	public:
		static void Initialize(const ResourceRegistryInitDesc& desc) noexcept;

		[[nodiscard]] static BufferHandle CreateBuffer(const BufferDesc& desc) noexcept;
		[[nodiscard]] static BufferHandle CreateMappedBuffer(const MappedBufferDesc& desc) noexcept;

		[[nodiscard]] static MappedBufferView ResolveMappedBuffer(BufferHandle handle) noexcept;

		static void FlushMappedBuffer(BufferHandle handle, DeviceSize offset, DeviceSize size) noexcept;

		static void Destroy(BufferHandle handle) noexcept;
		static void Destroy(TextureHandle handle) noexcept;

		[[nodiscard]] static TextureHandle CreateTexture(const TextureDesc& desc) noexcept;
	};

	template<typename T>
	[[nodiscard]] T* ResolveMappedBufferAs(BufferHandle handle) noexcept
	{
		const auto view = ResourceRegistry::ResolveMappedBuffer(handle);
		return static_cast<T*>(view.mappedPtr);
	}
} // namespace aether::gpu
