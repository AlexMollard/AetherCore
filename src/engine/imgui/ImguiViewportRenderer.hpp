#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include "vulkan/volk.hpp"

namespace aether
{
	class VulkanContext;
	class ImguiFrameData;

	// Render-thread-owned renderer for ImGui secondary (torn-out OS-window) viewports.
	// Owns one ImGui pipeline (built from ImGui's own shaders, layout-compatible with the
	// backend's AddTexture descriptor sets) and one ImGui_ImplVulkanH_Window per viewport.
	// All methods run on the render thread EXCEPT RetireViewports, which the producer calls
	// inside a RunExclusive quiesce (render thread parked, GPU idle).
	class ImguiViewportRenderer
	{
	public:
		ImguiViewportRenderer() = default;
		~ImguiViewportRenderer();

		ImguiViewportRenderer(const ImguiViewportRenderer&) = delete;
		ImguiViewportRenderer& operator=(const ImguiViewportRenderer&) = delete;

		// colorFormat MUST equal the main swapchain format so secondary swapchains share
		// this pipeline.
		void Init(VulkanContext& vk, VkFormat colorFormat);
		void Shutdown();

		// Render + present every secondary viewport in the snapshot (render thread).
		void Render(const ImguiFrameData& frame);

		// Destroy swapchains/surfaces for viewports whose ids are no longer present.
		// Producer thread, inside RunExclusive only.
		void RetireViewports(const std::vector<ImGuiID>& departedIds);

		[[nodiscard]] bool IsInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }

	private:
		// Host-visible vertex/index buffers for one swapchain image.
		struct FrameBuffers
		{
			VkDeviceMemory vtxMem = VK_NULL_HANDLE;
			VkDeviceMemory idxMem = VK_NULL_HANDLE;
			VkBuffer vtx = VK_NULL_HANDLE;
			VkBuffer idx = VK_NULL_HANDLE;
			VkDeviceSize vtxSize = 0;
			VkDeviceSize idxSize = 0;
		};

		struct PerViewport
		{
			ImGui_ImplVulkanH_Window window{};
			bool created = false;
			std::vector<FrameBuffers> ring; // one entry per swapchain image, indexed by acquired image
		};

		void CreatePipeline(VkFormat colorFormat);
		void EnsureWindow(PerViewport& vp, void* glfwWindow, int width, int height);
		void RenderOne(PerViewport& vp, const ImDrawData& draw);
		void DestroyOne(PerViewport& vp);
		void CreateOrResizeBuffer(VkBuffer& buffer, VkDeviceMemory& memory, VkDeviceSize& size, VkDeviceSize newSize, VkBufferUsageFlags usage) const;

		VulkanContext* m_vk = nullptr;
		VkDevice m_device = VK_NULL_HANDLE;
		VkFormat m_colorFormat = VK_FORMAT_UNDEFINED;

		VkSampler m_sampler = VK_NULL_HANDLE;
		VkDescriptorSetLayout m_texSetLayout = VK_NULL_HANDLE;     // set 0 (SAMPLED_IMAGE), matches AddTexture
		VkDescriptorSetLayout m_samplerSetLayout = VK_NULL_HANDLE; // set 1 (SAMPLER)
		VkDescriptorPool m_samplerPool = VK_NULL_HANDLE;
		VkDescriptorSet m_samplerDS = VK_NULL_HANDLE;             // shared sampler set, bound at set 1
		VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
		VkPipeline m_pipeline = VK_NULL_HANDLE;

		std::unordered_map<ImGuiID, PerViewport> m_viewports;
	};
} // namespace aether
