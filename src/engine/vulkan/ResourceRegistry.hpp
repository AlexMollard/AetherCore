#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>
#include <vk_mem_alloc.h>
#include "vulkan/volk.hpp"

#include "gpu/GpuHandles.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	class BindlessManager;
	class GpuMemoryTracker;
} // namespace aether

namespace aether
{
	// Thread safety: none. Create / Destroy / AdvanceFrame / DrainAll /
	class ResourceRegistry
	{
	public:
		static constexpr std::uint32_t kMaxFramesInFlight = 3;

		static constexpr int kAllocFrames = 4;
		static constexpr int kBacktraceDepth = 9;

		struct TextureEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageView storageView = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;
			VkExtent2D extent{};
			VkImageUsageFlags usage = 0;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			VkDevice device = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocator allocator = VK_NULL_HANDLE;
			bool ownsAllocation = false;
			bool ownsView = true;
			bool ownsStorageView = false;
			std::uint32_t mipLevels = 1;
			std::uint32_t arrayLayers = 1;
			// resource heap. Stable for the texture's lifetime.
			VkImageViewCreateInfo viewCreateInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			};
			static constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;
			std::uint32_t bindlessSampledSlot = kInvalidBindlessSlot;
			bool hasBindlessSampled = false;
			// False when the slot was reserved by somebody else and only pointed at this
			// texture, which is what lets a pooled render-graph transient keep one slot
			// across every image that backs it. Destroying the texture must not free it.
			bool ownsBindlessSlot = false;
		};

		struct BufferEntry
		{
			VkBuffer buffer = VK_NULL_HANDLE;
			VkDevice device = VK_NULL_HANDLE;
			VmaAllocation allocation = VK_NULL_HANDLE;
			VmaAllocator allocator = VK_NULL_HANDLE;
			VkBufferUsageFlags2 usage = 0;
			VkDeviceSize size = 0;
			bool ownsAllocation = false;
			void* mappedPtr = nullptr;
			VkDeviceAddress deviceAddress = 0;
		};

		struct PipelineEntry
		{
			VkDevice device = VK_NULL_HANDLE;

			VkShaderEXT vertexShader = VK_NULL_HANDLE;
			VkShaderEXT fragmentShader = VK_NULL_HANDLE;
			VkShaderEXT computeShader = VK_NULL_HANDLE;

			bool isGraphics = false;

			VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
			VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
			VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			VkBool32 depthClampEnable = VK_FALSE;
			VkBool32 rasterizerDiscardEnable = VK_FALSE;
			VkBool32 depthBiasEnable = VK_FALSE;
			VkBool32 primitiveRestartEnable = VK_FALSE;
			VkBool32 depthTestEnable = VK_FALSE;
			VkBool32 depthWriteEnable = VK_FALSE;
			VkCompareOp depthCompareOp = VK_COMPARE_OP_LESS;
			VkBool32 depthBoundsTestEnable = VK_FALSE;
			VkBool32 stencilTestEnable = VK_FALSE;
			VkLogicOp logicOp = VK_LOGIC_OP_COPY;
			float blendConstants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
			VkSampleCountFlags rasterizationSampleCount = VK_SAMPLE_COUNT_1_BIT;
			VkSampleMask sampleMask = 0xFFFFFFFFu;
			VkBool32 alphaToCoverageEnable = VK_FALSE;
			VkBool32 alphaToOneEnable = VK_FALSE;

			std::uint32_t scissorCount = 1;
			VkRect2D scissors[8] = {{{0, 0}, {1, 1}}};

			std::uint32_t viewportCount = 1;
			VkViewport viewports[8] = {{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}};

			VkBool32 colorBlendEnable = VK_FALSE;
			VkColorBlendEquationEXT colorBlendEquation{};
			VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			float lineWidth = 1.0f;
			bool hasLineWidth = false;

			std::vector<VkVertexInputBindingDescription2EXT> vertexBindings;
			std::vector<VkVertexInputAttributeDescription2EXT> vertexAttributes;
		};

		ResourceRegistry() = default;
		~ResourceRegistry();

		ResourceRegistry(const ResourceRegistry&) = delete;
		ResourceRegistry& operator=(const ResourceRegistry&) = delete;

		void Shutdown();

		gpu::TextureHandle RegisterTexture(const TextureEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		gpu::BufferHandle RegisterBuffer(const BufferEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		gpu::PipelineHandle RegisterPipeline(const PipelineEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		void Destroy(gpu::TextureHandle handle);
		void Destroy(gpu::BufferHandle handle);
		void Destroy(gpu::PipelineHandle handle);

		[[nodiscard]] const TextureEntry* Resolve(gpu::TextureHandle handle) const;
		[[nodiscard]] const BufferEntry* Resolve(gpu::BufferHandle handle) const;
		[[nodiscard]] const PipelineEntry* Resolve(gpu::PipelineHandle handle) const;

		[[nodiscard]] TextureEntry* ResolveMutable(gpu::TextureHandle handle);
		[[nodiscard]] BufferEntry* ResolveMutable(gpu::BufferHandle handle);

		void AdvanceFrame();

		void DrainAll();

		void Init(VkDevice device, VmaAllocator allocator) noexcept;

		// Queue families that access buffers (graphics/compute/transfer). When more than
		// one distinct family is supplied, buffers are created VK_SHARING_MODE_CONCURRENT
		// so transfer-queue uploads need no queue-family ownership transfer barriers
		// (concurrent sharing costs nothing measurable for buffers). Images stay
		// exclusive - they never touch the transfer queue (host image copy is CPU-side).
		void SetSharedBufferQueueFamilies(std::initializer_list<std::uint32_t> families) noexcept;

		[[nodiscard]] gpu::BufferHandle CreateBuffer(const gpu::BufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;
		[[nodiscard]] gpu::BufferHandle CreateMappedBuffer(const gpu::MappedBufferDesc& desc, std::source_location loc = std::source_location::current()) noexcept;
		[[nodiscard]] gpu::TextureHandle CreateTexture(const gpu::TextureDesc& desc, std::source_location loc = std::source_location::current()) noexcept;

		[[nodiscard]] gpu::MappedBufferView ResolveMappedBuffer(gpu::BufferHandle handle) const noexcept;
		void FlushMappedBuffer(gpu::BufferHandle handle, gpu::DeviceSize offset, gpu::DeviceSize size) noexcept;
		void InvalidateMappedBuffer(gpu::BufferHandle handle, gpu::DeviceSize offset, gpu::DeviceSize size) noexcept;

		void SetBufferName(gpu::BufferHandle handle, const char* name);
		void SetTextureName(gpu::TextureHandle handle, const char* name);

		[[nodiscard]] gpu::BufferHandle CreateAliasedBuffer(VkDeviceSize size, VkBufferUsageFlags2 usage, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName = {});
		[[nodiscard]] gpu::TextureHandle CreateAliasedTexture(const gpu::TextureDesc& desc, VmaAllocation existingAllocation, VkDeviceSize memoryOffset, std::string_view debugName = {});

		void SetBindlessManager(BindlessManager* mgr);

		void SetMemoryTracker(GpuMemoryTracker* tracker);
		Expected<void> EnsureBindlessSampled(gpu::TextureHandle handle, gpu::ImageAspect aspectMask = gpu::ImageAspect::Color, gpu::ImageLayout descriptorLayout = gpu::ImageLayout::ShaderReadOnly);
		[[nodiscard]] bool HasBindlessSampled(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(gpu::TextureHandle handle) const;

		// A descriptor slot with no image behind it yet. The caller owns it and must hand it
		// back through ReleaseBindlessSampledSlot; destroying whatever texture happens to be
		// pointed at it does not free it.
		[[nodiscard]] Expected<std::uint32_t> ReserveBindlessSampledSlot();
		void ReleaseBindlessSampledSlot(std::uint32_t slot);

		// Re-points a caller-owned slot at a texture. The slot number is unchanged, so anything
		// that cached it keeps working while the image underneath is free to move.
		Expected<void> BindSampledToSlot(gpu::TextureHandle handle, std::uint32_t slot, gpu::ImageAspect aspectMask, gpu::ImageLayout descriptorLayout);

		[[nodiscard]] gpu::Format GetTextureFormat(gpu::TextureHandle handle) const;
		[[nodiscard]] gpu::Extent2D GetTextureExtent(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetTextureMipLevels(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetTextureArrayLayers(gpu::TextureHandle handle) const;
		[[nodiscard]] gpu::ImageUsage GetTextureUsage(gpu::TextureHandle handle) const;
		[[nodiscard]] std::vector<gpu::DebugTextureInfo> ListDebugTextures() const;
		[[nodiscard]] std::vector<gpu::DebugBufferInfo> ListDebugBuffers() const;
		[[nodiscard]] gpu::GpuMemoryReport QueryMemoryReport() const;
		// Resources destroyed but still held for the frames-in-flight ring.
		[[nodiscard]] std::uint32_t GetPendingDestructionCount() const;

		[[nodiscard]] gpu::DeviceSize GetBufferSize(gpu::BufferHandle handle) const;
		[[nodiscard]] gpu::BufferUsage GetBufferUsage(gpu::BufferHandle handle) const;
		[[nodiscard]] const void* GetViewCreateInfo(gpu::TextureHandle handle) const noexcept;

	private:
		// ownership flags. We use a std::function to keep the type simple
		using DestructionFn = std::function<void()>;

		struct PendingDestruction
		{
			DestructionFn fn;
		};

		// size of the bool + alignment padding. Generation lives next to the
#ifndef NDEBUG
		struct AllocFrame
		{
			std::source_location site;
			std::array<void*, kBacktraceDepth> addresses{};
			int frameCount = 0;
		};
#endif

		struct TextureSlot
		{
			std::uint32_t generation = 1; // start at 1 so 0 is "never used"
			std::optional<TextureEntry> entry;
			std::string debugName;
#ifndef NDEBUG
			std::array<AllocFrame, kAllocFrames> allocFrames{};
			int allocSiteCount = 0;
#endif
		};

		struct BufferSlot
		{
			std::uint32_t generation = 1;
			std::optional<BufferEntry> entry;
			std::string debugName;
#ifndef NDEBUG
			std::array<AllocFrame, kAllocFrames> allocFrames{};
			int allocSiteCount = 0;
#endif
		};

		struct PipelineSlot
		{
			std::uint32_t generation = 1;
			std::optional<PipelineEntry> entry;
			std::string debugName;
#ifndef NDEBUG
			std::array<AllocFrame, kAllocFrames> allocFrames{};
			int allocSiteCount = 0;
#endif
		};

		[[nodiscard]] std::uint32_t AcquireTextureSlot();
		[[nodiscard]] std::uint32_t AcquireBufferSlot();
		[[nodiscard]] std::uint32_t AcquirePipelineSlot();

		static void RunDestroyersInRing(std::vector<PendingDestruction>& ring);
		static void DestroyTextureEntryNow(const TextureEntry& entry);
		static void DestroyBufferEntryNow(const BufferEntry& entry, GpuMemoryTracker* memoryTracker);
		static void DestroyPipelineEntryNow(const PipelineEntry& entry);

		std::vector<TextureSlot> m_textures;
		std::vector<BufferSlot> m_buffers;
		std::vector<PipelineSlot> m_pipelines;

		std::vector<std::uint32_t> m_freeTextureSlots;
		std::vector<std::uint32_t> m_freeBufferSlots;
		std::vector<std::uint32_t> m_freePipelineSlots;

		std::array<std::vector<PendingDestruction>, kMaxFramesInFlight> m_pendingDestructions;
		std::uint32_t m_currentFrame = 0;

		bool m_shutdown = false;

		VkDevice m_device = VK_NULL_HANDLE;
		VmaAllocator m_allocator = VK_NULL_HANDLE;
		std::uint32_t m_sharedBufferFamilies[3] = {};
		std::uint32_t m_sharedBufferFamilyCount = 0;
		BindlessManager* m_bindlessManager = nullptr;
		GpuMemoryTracker* m_memoryTracker = nullptr;

		std::uint32_t m_liveTextureCount = 0;
		std::uint32_t m_liveBufferCount = 0;
		std::uint32_t m_livePipelineCount = 0;
	};
} // namespace aether
