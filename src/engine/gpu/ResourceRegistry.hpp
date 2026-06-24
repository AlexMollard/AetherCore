#pragma once

#include <cstdint>
#include <source_location>
#include <span>

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
		// Component swizzle applied at view-creation time. Mirrors
		// VkComponentMapping (4 VkComponentSwizzle entries: r, g, b, a).
		// Defaults to identity swizzle. Useful for expanding R8_UNORM to
		// RGBA8 in the bindless descriptor (e.g. font SDF).
		gpu::ComponentSwizzle r = gpu::ComponentSwizzle::Identity;
		gpu::ComponentSwizzle g = gpu::ComponentSwizzle::Identity;
		gpu::ComponentSwizzle b = gpu::ComponentSwizzle::Identity;
		gpu::ComponentSwizzle a = gpu::ComponentSwizzle::Identity;
		const char* debugName = nullptr;
	};

	// Compute-pipeline description. Lives in the gpu/ facade so passes do not
	// need to know about vkutil::ComputePipelineDesc. The backend factory
	// mirrors this struct field-for-field.
	struct ComputePipelineDesc
	{
		const char* shaderVfsPath = nullptr;
		const char* shaderEntry = "main";
		const char* debugName = nullptr;
		const void* descriptorHeapMappings = nullptr;
	};

	// Graphics-pipeline description. Mirrors rendering::GraphicsPipeline::Desc
	// but lives in the gpu/ facade so ResourceRegistry callers do not need to
	// depend on the rendering layer. The facade translates to the rendering
	// type when forwarding to the Vulkan factory.
	struct GraphicsPipelineDesc
	{
		const char* shaderVfsPath = nullptr;
		// Optional separate fragment path. When non-null, the fragment SPIR-V
		// is loaded from this file instead of sharing the vertex module. The
		// default (nullptr) keeps the existing single-module behavior.
		const char* fragmentVfsPath = nullptr;
		const char* vertexEntry = "vertexMain";
		const char* fragmentEntry = "fragmentMain";
		Format colorFormat = Format::Undefined;
		Format depthFormat = Format::Undefined;
		bool depthTestEnable = false;
		bool depthWriteEnable = false;
		CompareOp depthCompareOp = CompareOp::Less;
		bool blendEnable = false;
		// Graphics-pipeline state overrides for non-default pipelines
		// (debug renderers, etc.). Defaults match the standard MeshDraw path.
		PrimitiveTopology topology = PrimitiveTopology::TriangleList;
		PolygonMode polygonMode = PolygonMode::Fill;
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

	class ResourceRegistry
	{
	public:
		static void Initialize(const ResourceRegistryInitDesc& desc) noexcept;

		[[nodiscard]] static BufferHandle CreateBuffer(const BufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;
		[[nodiscard]] static BufferHandle CreateMappedBuffer(const MappedBufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;

		[[nodiscard]] static MappedBufferView ResolveMappedBuffer(BufferHandle handle) noexcept;

		static void FlushMappedBuffer(BufferHandle handle, DeviceSize offset, DeviceSize size) noexcept;

		static void Destroy(BufferHandle handle) noexcept;
		static void Destroy(TextureHandle handle) noexcept;
		static void Destroy(PipelineHandle handle) noexcept;

		[[nodiscard]] static TextureHandle CreateTexture(const TextureDesc& desc, std::source_location loc = std::source_location::current()) noexcept;

		// Aliased creation (transient heap sub-allocation). The existingAllocation
		// pointer (VmaAllocation) is cast through void* at the facade.
		[[nodiscard]] static TextureHandle CreateAliasedTexture(const TextureDesc& desc, void* existingAllocation, DeviceSize memoryOffset, const char* debugName = nullptr) noexcept;
		[[nodiscard]] static BufferHandle CreateAliasedBuffer(DeviceSize size, BufferUsage usage, void* existingAllocation, DeviceSize memoryOffset, const char* debugName = nullptr) noexcept;

		// Compute-pipeline: factory + register. Returns an opaque
		// PipelineHandle.
		[[nodiscard]] static PipelineHandle CreateComputePipeline(Device device, const ComputePipelineDesc& desc) noexcept;

		// Graphics-pipeline: factory + register. Returns an opaque
		// PipelineHandle bound to one or two VkShaderEXT handles plus the
		// cached dynamic state the command list re-applies on every bind.
		[[nodiscard]] static PipelineHandle CreateGraphicsPipeline(Device device, const struct GraphicsPipelineDesc& desc) noexcept;

		// Resolve a PipelineHandle to an opaque pointer to the vulkan-side
		// pipeline state (shader handles + cached dynamic state). The pointer
		// is valid until the handle is destroyed. Returns nullptr if the
		// handle is stale or the slot is empty.
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
		// Engine-side accessor: returns the texture image as an opaque
		// gpu::Image. Used at the render-graph boundary to register external
		// images. Backend resolves to the underlying VkImage at the seam.
		[[nodiscard]] static gpu::Image ResolveTextureImage(TextureHandle handle) noexcept;

		// Naming
		static void SetBufferName(BufferHandle handle, const char* name);
		static void SetTextureName(TextureHandle handle, const char* name);

		// Bindless support
		// EnsureBindlessSampled uses the BindlessManager pointer set via
		// SetBindlessManager for both slot allocation (here) and deferred
		// slot-free during Destroy.
		static void SetBindlessManager(class aether::BindlessManager* mgr);
		static void EnsureBindlessSampled(TextureHandle handle, ImageAspect aspectMask = ImageAspect::Color, ImageLayout descriptorLayout = ImageLayout::ShaderReadOnly);
		[[nodiscard]] static bool HasBindlessSampled(TextureHandle handle);
		[[nodiscard]] static std::uint32_t GetBindlessSampledSlot(TextureHandle handle);

		// Texture property queries
		[[nodiscard]] static Format GetTextureFormat(TextureHandle handle);
		[[nodiscard]] static Extent2D GetTextureExtent(TextureHandle handle);
		[[nodiscard]] static std::uint32_t GetTextureMipLevels(TextureHandle handle);
		[[nodiscard]] static std::uint32_t GetTextureArrayLayers(TextureHandle handle);
		[[nodiscard]] static ImageUsage GetTextureUsage(TextureHandle handle);

		// Buffer property queries
		[[nodiscard]] static DeviceSize GetBufferSize(BufferHandle handle);
		[[nodiscard]] static BufferUsage GetBufferUsage(BufferHandle handle);

		// Returns an opaque pointer to the VkImageViewCreateInfo used to create
		// the texture view. Null if the handle is stale. Needed by BindlessManager
		// WriteSampledImage for descriptor_heap VkImageDescriptorInfoEXT.pView.
		[[nodiscard]] static const void* GetViewCreateInfo(TextureHandle handle) noexcept;
	};
} // namespace aether::gpu
