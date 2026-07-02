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
	// Owns opaque handles for engine-facing GPU resources and centralizes
	// deferred destruction (kMaxFramesInFlight ring, WaitIdle-aware).
	//
	// Slot tables: TextureSlot, BufferSlot, PipelineSlot. Each slot is
	// {generation, std::optional<Entry>}. Generation increments on slot reuse
	// so stale handles fail validation in Resolve*().
	//
	// Deferred destruction: callers schedule destruction with Destroy(handle);
	// the entry is queued in the *current* frame's ring slot. AdvanceFrame()
	// promotes the ring by one slot and runs destroyers whose target frame has
	// retired (GpuDevice::WaitIdle at the matching point guarantees GPU done).
	//
	// Thread safety: none. Create / Destroy / AdvanceFrame / DrainAll /
	// Shutdown mutate internal slot tables and the pending-destruction ring
	// without locking. All calls must be externally serialized to a single
	// thread (the engine's main thread / dedicated render thread).
	class ResourceRegistry
	{
	public:
		static constexpr std::uint32_t kMaxFramesInFlight = 3;

		// Debug backtrace constants.
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
			// Recipe for VK_EXT_descriptor_heap: vkWriteResourceDescriptorsEXT
			// takes a const VkImageViewCreateInfo* (the recipe, not the view
			// handle). Stored here at view-creation time so the bindless
			// manager can re-issue it when registering the texture into the
			// resource heap. Stable for the texture's lifetime.
			VkImageViewCreateInfo viewCreateInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			};
			static constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;
			std::uint32_t bindlessSampledSlot = kInvalidBindlessSlot;
			bool hasBindlessSampled = false;
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

		// A pipeline is a set of VK_EXT_shader_object shader handles plus the
		// rasterization / depth / blend / topology state that was previously
		// baked into a VkPipeline. CommandList::BindPipeline re-applies the
		// cached state via vkCmdSet* on every bind (shader objects are fully
		// dynamic - no static pipeline state survives the migration).
		struct PipelineEntry
		{
			VkDevice device = VK_NULL_HANDLE;

			// Shader handles. For graphics: vertexShader is set, fragmentShader
			// is set unless the pipeline is depth-only (no fragment stage).
			// For compute: computeShader is set, the graphics fields are null.
			VkShaderEXT vertexShader = VK_NULL_HANDLE;
			VkShaderEXT fragmentShader = VK_NULL_HANDLE;
			VkShaderEXT computeShader = VK_NULL_HANDLE;

			bool isGraphics = false;

			// -- Cached dynamic state (graphics only) -------------------------
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
			// Sample mask + alpha-to-coverage / alpha-to-one. The engine uses 1
			// sample (no MSAA), so the sample mask is all-ones and alpha-to-* is off.
			VkSampleMask sampleMask = 0xFFFFFFFFu;
			VkBool32 alphaToCoverageEnable = VK_FALSE;
			VkBool32 alphaToOneEnable = VK_FALSE;

			std::uint32_t scissorCount = 1;
			VkRect2D scissors[8] = {{{0, 0}, {1, 1}}};

			std::uint32_t viewportCount = 1;
			VkViewport viewports[8] = {{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}};

			// One color attachment's blend state (engine uses at most 1 RT per pipeline).
			VkBool32 colorBlendEnable = VK_FALSE;
			VkColorBlendEquationEXT colorBlendEquation{};
			VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
			float lineWidth = 1.0f;
			bool hasLineWidth = false;

			// Vertex input (vertex-input-dynamic-state). Empty for BDA-only pipelines
			// (the common case); populated by debug renderers that bind vertex streams.
			std::vector<VkVertexInputBindingDescription2EXT> vertexBindings;
			std::vector<VkVertexInputAttributeDescription2EXT> vertexAttributes;
		};

		ResourceRegistry() = default;
		~ResourceRegistry();

		ResourceRegistry(const ResourceRegistry&) = delete;
		ResourceRegistry& operator=(const ResourceRegistry&) = delete;

		void Shutdown();

		// Texture registration. Pass ownsAllocation=true to have the registry destroy on Destroy().
		gpu::TextureHandle RegisterTexture(const TextureEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		// Buffer registration.
		gpu::BufferHandle RegisterBuffer(const BufferEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		// Pipeline registration.
		gpu::PipelineHandle RegisterPipeline(const PipelineEntry& entry, std::string_view debugName = {}, std::source_location loc = std::source_location::current());

		// Schedule destruction. Runs kMaxFramesInFlight frames later in AdvanceFrame.
		void Destroy(gpu::TextureHandle handle);
		void Destroy(gpu::BufferHandle handle);
		void Destroy(gpu::PipelineHandle handle);

		// Resolve handle -> entry, or nullptr if stale.
		[[nodiscard]] const TextureEntry* Resolve(gpu::TextureHandle handle) const;
		[[nodiscard]] const BufferEntry* Resolve(gpu::BufferHandle handle) const;
		[[nodiscard]] const PipelineEntry* Resolve(gpu::PipelineHandle handle) const;

		[[nodiscard]] TextureEntry* ResolveMutable(gpu::TextureHandle handle);
		[[nodiscard]] BufferEntry* ResolveMutable(gpu::BufferHandle handle);

		// Advance the deferred-destruction ring by one frame.
		void AdvanceFrame();

		// Run all queued destroyers while the device is still valid.
		void DrainAll();

		void Init(VkDevice device, VmaAllocator allocator) noexcept;

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

		// Bindless registration. BindlessManager pointer is for deferred slot-free on Destroy().
		void SetBindlessManager(BindlessManager* mgr);

		// Diagnostic address tracking. GpuMemoryTracker pointer lets the
		// registry auto-register every BDA range so the DiagnosticEngine can
		// resolve raw GPU fault addresses back to resource names.
		void SetMemoryTracker(GpuMemoryTracker* tracker);
		Expected<void> EnsureBindlessSampled(gpu::TextureHandle handle, gpu::ImageAspect aspectMask = gpu::ImageAspect::Color, gpu::ImageLayout descriptorLayout = gpu::ImageLayout::ShaderReadOnly);
		[[nodiscard]] bool HasBindlessSampled(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetBindlessSampledSlot(gpu::TextureHandle handle) const;

		[[nodiscard]] gpu::Format GetTextureFormat(gpu::TextureHandle handle) const;
		[[nodiscard]] gpu::Extent2D GetTextureExtent(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetTextureMipLevels(gpu::TextureHandle handle) const;
		[[nodiscard]] std::uint32_t GetTextureArrayLayers(gpu::TextureHandle handle) const;
		[[nodiscard]] gpu::ImageUsage GetTextureUsage(gpu::TextureHandle handle) const;
		[[nodiscard]] std::vector<gpu::DebugTextureInfo> ListDebugTextures() const;

		[[nodiscard]] gpu::DeviceSize GetBufferSize(gpu::BufferHandle handle) const;
		[[nodiscard]] gpu::BufferUsage GetBufferUsage(gpu::BufferHandle handle) const;
		[[nodiscard]] const void* GetViewCreateInfo(gpu::TextureHandle handle) const noexcept;

	private:
		// A queued destructor. Holds a typed-owning variant of the resource
		// payload (or a closure) so destruction is unambiguous regardless of
		// ownership flags. We use a std::function to keep the type simple
		// at the cost of a small heap allocation per Destroy() call; this is
		// cold path (shaders/transients), not the steady-state hot loop.
		using DestructionFn = std::function<void()>;

		struct PendingDestruction
		{
			DestructionFn fn;
		};

		// Per-slot storage. std::optional so an empty slot costs only the
		// size of the bool + alignment padding. Generation lives next to the
		// payload so any reuse bumps the generation atomically.
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
			int allocSiteCount = 0; // total pushes (may exceed kAllocFrames)
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

		// Find an empty slot, or pick a victim for reuse. Returns ~0u when
		// the table is full (16-bit index space exhausted).
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
		BindlessManager* m_bindlessManager = nullptr;
		GpuMemoryTracker* m_memoryTracker = nullptr;

		std::uint32_t m_liveTextureCount = 0;
		std::uint32_t m_liveBufferCount = 0;
		std::uint32_t m_livePipelineCount = 0;
	};
} // namespace aether
