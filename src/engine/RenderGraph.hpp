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

	// A Frostbite-style frame graph.
	//
	// Passes declare resource reads and writes through a fluent PassBuilder;
	// the graph derives image layouts and issues the minimum required pipeline
	// barriers automatically.  Two resources are always pre-registered:
	//   GetSwapchainColor() — the current swapchain color image
	//   GetSwapchainDepth() — the swapchain depth image
	//
	// Example usage (setup, called once):
	//   auto color = graph.GetSwapchainColor();
	//   auto depth = graph.GetSwapchainDepth();
	//
	//   graph.AddPass("ForwardPass")
	//       .WriteColor(color, VK_ATTACHMENT_LOAD_OP_CLEAR,
	//       VK_ATTACHMENT_STORE_OP_STORE,
	//                   ClearColorValue(0.05f, 0.05f, 0.07f, 1.0f))
	//       .WriteDepth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR,
	//       VK_ATTACHMENT_STORE_OP_DONT_CARE,
	//                   ClearDepthValue(1.0f))
	//       .Execute([](PassContext& ctx) { /* record draw calls */ });
	class RenderGraph
	{
	public:
		// ── Fluent builder returned by AddPass() ─────────────────────────────
		// Chain calls to declare attachments, then call Execute() to set the
		// callback. The builder references the graph — consume it immediately (do not
		// store).
		class PassBuilder
		{
		public:
			PassBuilder(PassBuilder&&) = default;
			PassBuilder(const PassBuilder&) = delete;
			PassBuilder& operator=(PassBuilder&&) = delete;

			// Declare a color attachment write.
			// clearValue is used only when loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR.
			PassBuilder& WriteColor(RGImage image, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_STORE, VkClearValue clearValue = {});

			// Declare the depth/stencil attachment write.
			// clearValue is used only when loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR.
			PassBuilder& WriteDepth(RGImage image, VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, VkAttachmentStoreOp storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, VkClearValue clearValue = {});

			// Declare a shader-sampled texture read.
			// Generates a layout transition to SHADER_READ_ONLY_OPTIMAL before the
			// pass.
			PassBuilder& ReadTexture(RGImage image);

			// Declare a shader-sampled texture read used by compute.
			// Generates a layout transition to SHADER_READ_ONLY_OPTIMAL before the
			// pass.
			PassBuilder& ReadTextureCompute(RGImage image);

			// Declare a storage-image read used by compute.
			// Generates a layout transition to GENERAL before the pass.
			PassBuilder& ReadStorageImage(RGImage image);

			// Declare a storage-image write used by compute.
			// Generates a layout transition to GENERAL before the pass.
			PassBuilder& WriteStorageImage(RGImage image);

			// Set the GPU work callback for this pass.
			// Called between vkCmdBeginRendering and vkCmdEndRendering.
			PassBuilder& Execute(std::function<void(PassContext&)> fn);

			// Set the GPU work callback for a compute pass.
			// Called without opening dynamic rendering.
			PassBuilder& ExecuteCompute(std::function<void(PassContext&)> fn);

			// Override the render area / viewport / scissor for this pass.
			// When not set the pass renders at the swapchain extent.
			// Required for render-to-texture passes whose target is not
			// swapchain-sized.
			PassBuilder& SetExtent(VkExtent2D extent);

		private:
			friend class RenderGraph;
			PassBuilder(RenderGraph& graph, std::size_t passIndex);

			RenderGraph& m_graph;
			std::size_t m_passIndex;
		};

		// ── Pre-registered swapchain resource handles ─────────────────────────
		[[nodiscard]] RGImage GetSwapchainColor() const
		{
			return RGImage{ kSwapchainColorId };
		}

		[[nodiscard]] RGImage GetSwapchainDepth() const
		{
			return RGImage{ kSwapchainDepthId };
		}

		// Register an externally-owned image (VkImage + VkImageView) with the graph.
		// The caller is responsible for the lifetime of the Vulkan handles — they
		// must remain valid for all Execute calls made while the registration is
		// live. Returns an opaque handle for use with
		// WriteColor/WriteDepth/ReadTexture. Pass VK_IMAGE_ASPECT_DEPTH_BIT for
		// depth/stencil images.
		[[nodiscard]] RGImage RegisterImage(VkImage image, VkImageView view, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

		// ── Pass management ───────────────────────────────────────────────────
		// Registers a pass and returns a builder for declaring its resource accesses.
		// The builder should be consumed immediately via method chaining.
		[[nodiscard]] PassBuilder AddPass(std::string name);

		// Registers a compute pass and returns a builder for declaring its resource
		// accesses. Compute passes execute without dynamic rendering.
		[[nodiscard]] PassBuilder AddComputePass(std::string name);

		void RemovePass(const std::string& name);
		[[nodiscard]] bool HasPass(std::string_view name) const;
		void Clear();

		[[nodiscard]] bool IsEmpty() const
		{
			return m_passes.empty();
		}

		// ── Frame execution ───────────────────────────────────────────────────
		// Compiles the graph (if dirty), issues pre-pass barriers, opens/closes
		// dynamic rendering, and invokes each pass's Execute callback.
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
