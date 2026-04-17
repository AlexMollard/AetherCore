#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

#include "CommandRecorder.hpp"

namespace aether
{
	// Opaque handle to a render-graph-managed image resource.
	// Acquired from RenderGraph::GetSwapchainColor/Depth or future
	// CreateTransient*.
	struct RGImage
	{
		static constexpr uint32_t kInvalid = ~0u;
		uint32_t id = kInvalid;

		[[nodiscard]] bool IsValid() const
		{
			return id != kInvalid;
		}
	};

	// Helpers for constructing VkClearValue without nested brace issues on MSVC.
	[[nodiscard]] inline VkClearValue ClearColorValue(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f) noexcept
	{
		VkClearValue v{};
		v.color.float32[0] = r;
		v.color.float32[1] = g;
		v.color.float32[2] = b;
		v.color.float32[3] = a;
		return v;
	}

	[[nodiscard]] inline VkClearValue ClearDepthValue(float depth = 1.0f, uint32_t stencil = 0u) noexcept
	{
		VkClearValue v{};
		v.depthStencil.depth = depth;
		v.depthStencil.stencil = stencil;
		return v;
	}

	// Data made available inside pass execute callbacks.
	struct PassContext
	{
		CommandRecorder& recorder;
		VkExtent2D extent;
		VkDeviceAddress frameConstantsAddr;
		std::uint32_t frameIndex = 0;
	};

	// Per-frame swapchain handles supplied to RenderGraph::Execute by AetherCore.
	struct FrameTarget
	{
		VkImage colorImage = VK_NULL_HANDLE;
		VkImageView colorView = VK_NULL_HANDLE;
		VkImage depthImage = VK_NULL_HANDLE;
		VkImageView depthView = VK_NULL_HANDLE;
		VkFormat colorFormat = VK_FORMAT_UNDEFINED;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D extent{};
	};

	// Frame graph with pass/resource declarations and automatic image barriers.
	class RenderGraph
	{
	public:
		// Fluent pass builder; use immediately, do not store.
		class PassBuilder
		{
		public:
			PassBuilder(PassBuilder&&) = default;
			PassBuilder(const PassBuilder&) = delete;
			PassBuilder& operator=(PassBuilder&&) = delete;

			// Declare a color attachment write.
			PassBuilder& WriteColor(RGImage image, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE, VkClearValue clearValue = {});

			// Declare a depth/stencil attachment write.
			PassBuilder& WriteDepth(RGImage image, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, VkClearValue clearValue = {});

			// Declare a sampled texture read.
			PassBuilder& ReadTexture(RGImage image);

			// Declare a sampled texture read in compute.
			PassBuilder& ReadTextureCompute(RGImage image);

			// Declare a storage-image read in compute.
			PassBuilder& ReadStorageImage(RGImage image);

			// Declare a storage-image write in compute.
			PassBuilder& WriteStorageImage(RGImage image);

			// Set graphics callback for this pass.
			PassBuilder& Execute(std::function<void(PassContext&)> fn);

			// Set compute callback for this pass.
			PassBuilder& ExecuteCompute(std::function<void(PassContext&)> fn);

			// Override pass extent (for render-to-texture and non-swapchain targets).
			PassBuilder& SetExtent(VkExtent2D extent);

		private:
			friend class RenderGraph;
			PassBuilder(RenderGraph& graph, std::size_t passIndex);

			RenderGraph& m_graph;
			std::size_t m_passIndex;
		};

		[[nodiscard]] RGImage GetSwapchainColor() const
		{
			return RGImage{ kSwapchainColorId };
		}

		[[nodiscard]] RGImage GetSwapchainDepth() const
		{
			return RGImage{ kSwapchainDepthId };
		}

		// Register an externally-owned image and return an RGImage handle.
		[[nodiscard]] RGImage RegisterImage(VkImage image, VkImageView view, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

		// Register a graphics pass.
		[[nodiscard]] PassBuilder AddPass(std::string name);

		// Register a compute pass.
		[[nodiscard]] PassBuilder AddComputePass(std::string name);

		void RemovePass(const std::string& name);
		[[nodiscard]] bool HasPass(std::string_view name) const;
		void Clear();

		[[nodiscard]] bool IsEmpty() const
		{
			return m_passes.empty();
		}

		// Execute the compiled frame graph for the current frame.
		void Execute(VkCommandBuffer cmd, const FrameTarget& target, VkDeviceAddress frameConstantsAddr, std::uint32_t frameIndex);

	private:
		static constexpr uint32_t kSwapchainColorId = 0u;
		static constexpr uint32_t kSwapchainDepthId = 1u;
		static constexpr uint32_t kFirstExternalId = 2u;

		// Entry for each image registered via RegisterImage().
		struct ExternalImageEntry
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		struct AttachmentRef
		{
			RGImage image{};
			VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkClearValue clearValue{};
		};

		enum class ImageAccessType
		{
			SampledRead,
			StorageRead,
			StorageWrite,
		};

		struct ImageAccessRef
		{
			RGImage image{};
			ImageAccessType type = ImageAccessType::SampledRead;
		};

		enum class PassKind
		{
			Graphics,
			Compute,
		};

		struct PassRecord
		{
			std::string name;
			PassKind kind = PassKind::Graphics;
			std::vector<AttachmentRef> colorWrites;
			std::optional<AttachmentRef> depthWrite;
			std::vector<ImageAccessRef> imageAccesses;
			std::function<void(PassContext&)> execute;
			std::optional<VkExtent2D> extentOverride; // if set, overrides target.extent
		};

		struct CompiledBarrier
		{
			uint32_t resourceId = 0;
			VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkPipelineStageFlags2 srcStage = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 srcAccess = VK_ACCESS_2_NONE;
			VkPipelineStageFlags2 dstStage = VK_PIPELINE_STAGE_2_NONE;
			VkAccessFlags2 dstAccess = VK_ACCESS_2_NONE;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		};

		struct CompiledPass
		{
			std::size_t passIndex = 0;
			std::vector<CompiledBarrier> preBarriers;
		};

		void Compile();

		[[nodiscard]] VkImage ResolveImage(uint32_t resourceId, const FrameTarget& target) const;
		[[nodiscard]] VkImageView ResolveView(uint32_t resourceId, const FrameTarget& target) const;
		[[nodiscard]] VkImageAspectFlags ResolveAspect(uint32_t resourceId) const;

		std::vector<PassRecord> m_passes;
		std::vector<CompiledPass> m_compiled;
		std::vector<ExternalImageEntry> m_externalImages; // indexed by (id - kFirstExternalId)
		bool m_dirty = true;
	};
} // namespace aether
