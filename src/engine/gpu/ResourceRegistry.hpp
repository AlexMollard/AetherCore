#pragma once

#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <vector>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class BindlessManager;
}

namespace aether::gpu
{
	struct BufferDesc
	{
		DeviceSize size = 0;
		BufferUsage usage = BufferUsage::None;
		const char* debugName = nullptr;
	};

	struct MappedBufferDesc
	{
		DeviceSize size = 0;
		BufferUsage usage = BufferUsage::None;
		MappedMemoryUsage memoryUsage = MappedMemoryUsage::Auto;
		const char* debugName = nullptr;
	};

	struct MappedBufferView
	{
		void* mappedPtr = nullptr;
		DeviceAddress deviceAddress = 0;
		DeviceSize size = 0;
	};

	struct TextureDesc
	{
		Format format = Format::Undefined;
		Extent2D extent;
		ImageUsage usage = ImageUsage::None;
		ImageAspect aspect = ImageAspect::Color;
		std::uint32_t mipLevels = 1;
		std::uint32_t arrayLayers = 1;
		gpu::ComponentSwizzle r = gpu::ComponentSwizzle::Identity;
		gpu::ComponentSwizzle g = gpu::ComponentSwizzle::Identity;
		gpu::ComponentSwizzle b = gpu::ComponentSwizzle::Identity;
		gpu::ComponentSwizzle a = gpu::ComponentSwizzle::Identity;
		const char* debugName = nullptr;
	};

	struct ComputePipelineDesc
	{
		const char* shaderVfsPath = nullptr;
		const char* shaderEntry = "main";
		const char* debugName = nullptr;
		const void* descriptorHeapMappings = nullptr;
	};

	struct GraphicsPipelineDesc
	{
		const char* shaderVfsPath = nullptr;
		const char* fragmentVfsPath = nullptr;
		const char* vertexEntry = "vertexMain";
		const char* fragmentEntry = "fragmentMain";
		Format colorFormat = Format::Undefined;
		Format depthFormat = Format::Undefined;
		bool depthTestEnable = false;
		bool depthWriteEnable = false;
		CompareOp depthCompareOp = CompareOp::Less;
		bool blendEnable = false;
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		PolygonMode polygonMode = PolygonMode::Fill;
		CullMode cullMode = CullMode::None;
		std::span<const VertexInputBinding> vertexBindings;
		std::span<const VertexInputAttribute> vertexAttributes;
		bool lineWidthDynamic = false;
		const char* debugName = nullptr;
		const void* descriptorHeapMappings = nullptr;
	};

	struct ResourceRegistryInitDesc
	{
		void* vulkanDevice = nullptr;
		void* vmaAllocator = nullptr;
		void* backendRegistry = nullptr;
	};

	struct DebugTextureInfo
	{
		TextureHandle handle;
		ImageView view = nullptr;
		Format format = Format::Undefined;
		Extent2D extent;
		ImageUsage usage = ImageUsage::None;
		ImageAspect aspect = ImageAspect::None;
		std::uint32_t mipLevels = 0;
		std::uint32_t arrayLayers = 0;
		bool hasBindlessSampled = false;
		std::uint32_t bindlessSampledSlot = 0xFFFFFFFFu;
		std::string debugName;
	};

	class ResourceRegistry
	{
	public:
		static void Initialize(const ResourceRegistryInitDesc& desc) noexcept;

		[[nodiscard]] static BufferHandle CreateBuffer(const BufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;
		[[nodiscard]] static BufferHandle CreateMappedBuffer(const MappedBufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;

		[[nodiscard]] static MappedBufferView ResolveMappedBuffer(BufferHandle handle) noexcept;

		static void FlushMappedBuffer(BufferHandle handle, DeviceSize offset, DeviceSize size) noexcept;
		static void InvalidateMappedBuffer(BufferHandle handle, DeviceSize offset, DeviceSize size) noexcept;

		static void Destroy(BufferHandle handle) noexcept;
		static void Destroy(TextureHandle handle) noexcept;
		static void Destroy(PipelineHandle handle) noexcept;

		[[nodiscard]] static TextureHandle CreateTexture(const TextureDesc& desc, std::source_location loc = std::source_location::current()) noexcept;

		[[nodiscard]] static TextureHandle CreateAliasedTexture(const TextureDesc& desc, void* existingAllocation, DeviceSize memoryOffset, const char* debugName = nullptr) noexcept;
		[[nodiscard]] static BufferHandle CreateAliasedBuffer(DeviceSize size, BufferUsage usage, void* existingAllocation, DeviceSize memoryOffset, const char* debugName = nullptr) noexcept;

		[[nodiscard]] static PipelineHandle CreateComputePipeline(Device device, const ComputePipelineDesc& desc) noexcept;

		[[nodiscard]] static PipelineHandle CreateGraphicsPipeline(Device device, const struct GraphicsPipelineDesc& desc) noexcept;

		struct ResolvedPipeline
		{
			const void* state = nullptr;
		};

		struct ResolvedTexture
		{
			ImageView view = nullptr;
			Format format = Format::Undefined;
			Extent2D extent;
			std::uint32_t mipLevels = 0;
			std::uint32_t arrayLayers = 0;
			ImageUsage usage = ImageUsage::None;
		};

		struct ResolvedBuffer
		{
			DeviceAddress deviceAddress = 0;
			DeviceSize size = 0;
			BufferUsage usage = BufferUsage::None;
		};

		[[nodiscard]] static ResolvedPipeline ResolvePipeline(PipelineHandle handle) noexcept;
		[[nodiscard]] static ResolvedTexture ResolveTexture(TextureHandle handle) noexcept;
		[[nodiscard]] static ResolvedBuffer ResolveBuffer(BufferHandle handle) noexcept;
		[[nodiscard]] static void* ResolveBufferVkHandle(BufferHandle handle) noexcept;
		[[nodiscard]] static gpu::Image ResolveTextureImage(TextureHandle handle) noexcept;

		static void SetBufferName(BufferHandle handle, const char* name);
		static void SetTextureName(TextureHandle handle, const char* name);

		static void SetBindlessManager(class aether::BindlessManager* mgr);
		static void EnsureBindlessSampled(TextureHandle handle, ImageAspect aspectMask = ImageAspect::Color, ImageLayout descriptorLayout = ImageLayout::ShaderReadOnly);
		[[nodiscard]] static bool HasBindlessSampled(TextureHandle handle);
		[[nodiscard]] static std::uint32_t GetBindlessSampledSlot(TextureHandle handle);

		[[nodiscard]] static Format GetTextureFormat(TextureHandle handle);
		[[nodiscard]] static Extent2D GetTextureExtent(TextureHandle handle);
		[[nodiscard]] static std::uint32_t GetTextureMipLevels(TextureHandle handle);
		[[nodiscard]] static std::uint32_t GetTextureArrayLayers(TextureHandle handle);
		[[nodiscard]] static ImageUsage GetTextureUsage(TextureHandle handle);
		[[nodiscard]] static std::vector<DebugTextureInfo> ListDebugTextures();

		[[nodiscard]] static DeviceSize GetBufferSize(BufferHandle handle);
		[[nodiscard]] static BufferUsage GetBufferUsage(BufferHandle handle);

		[[nodiscard]] static const void* GetViewCreateInfo(TextureHandle handle) noexcept;
	};
} // namespace aether::gpu
