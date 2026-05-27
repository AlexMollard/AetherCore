#include "ui/ImGuiRenderer.hpp"

#ifdef AETHER_IMGUI

#	include <cstring>
#	include <mutex>
#	include <type_traits>

#	include <backends/imgui_impl_glfw.h>
#	include <backends/imgui_impl_vulkan.h>
#	include <GLFW/glfw3.h>

#	include "utils/ServiceContainer.hpp"
#	include "utils/Assert.hpp"
#	include "utils/Profiler.hpp"
#	include "rendering/RenderGraph.hpp"
#	include "vulkan/Swapchain.hpp"
#	include "vulkan/VulkanContext.hpp"

namespace aether
{

	// ── Queue serialization wrappers ───────────────────────────────────────────
	// imgui_impl_vulkan touches the VkQueue from two code paths that run on the
	// game thread concurrently with the render thread's EndFrame submission:
	//
	//   Game thread (RenderPlatformWindowsDefault):
	//     vkQueueSubmit          - secondary viewport command buffer submission
	//     vkQueuePresentKHR      - secondary viewport present
	//     vkDeviceWaitIdle       - ImGui_ImplVulkanH_CreateWindowSwapChain /
	//                              ImGui_ImplVulkanH_DestroyWindow
	//
	//   Render thread (inside RenderDrawData):
	//     vkQueueSubmit          - draw data upload
	//     vkQueueWaitIdle        - wait for upload
	//
	// AetherCore::EndFrame wraps m_swapchain.EndFrame() with g_queueMutex.
	// These wrappers intercept every imgui queue op and lock the same mutex so
	// all accesses to the VkQueue serialise correctly.
	namespace
	{
		// ── AetherCore ImGui theme ─────────────────────────────────────────────────
		static void ApplyAetherTheme()
		{
			ImGuiStyle& style = ImGui::GetStyle();

			style.WindowRounding = 8.0f;
			style.ChildRounding = 6.0f;
			style.PopupRounding = 6.0f;
			style.FrameRounding = 4.0f;
			style.ScrollbarRounding = 4.0f;
			style.GrabRounding = 4.0f;
			style.TabRounding = 4.0f;
			style.WindowBorderSize = 1.0f;
			style.FrameBorderSize = 0.0f;
			style.PopupBorderSize = 1.0f;
			style.TabBarBorderSize = 1.0f;

			style.WindowPadding = ImVec2(10.0f, 8.0f);
			style.FramePadding = ImVec2(6.0f, 4.0f);
			style.CellPadding = ImVec2(6.0f, 4.0f);
			style.ItemSpacing = ImVec2(8.0f, 6.0f);
			style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
			style.IndentSpacing = 20.0f;
			style.ScrollbarSize = 12.0f;
			style.GrabMinSize = 10.0f;

			ImVec4* c = style.Colors;

			c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.11f, 0.97f);
			c[ImGuiCol_ChildBg] = ImVec4(0.04f, 0.06f, 0.09f, 0.95f);
			c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.09f, 0.13f, 0.98f);
			c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.03f, 0.55f);

			c[ImGuiCol_Border] = ImVec4(0.18f, 0.23f, 0.30f, 0.80f);
			c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

			c[ImGuiCol_Text] = ImVec4(0.88f, 0.91f, 0.93f, 1.00f);
			c[ImGuiCol_TextDisabled] = ImVec4(0.38f, 0.46f, 0.54f, 1.00f);
			c[ImGuiCol_TextSelectedBg] = ImVec4(0.42f, 0.62f, 0.74f, 0.35f);

			c[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.09f, 0.13f, 1.00f);
			c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
			c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.06f, 0.09f, 0.80f);

			c[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);

			c[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.05f, 0.08f, 0.70f);
			c[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.26f, 0.34f, 1.00f);
			c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.36f, 0.46f, 1.00f);
			c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);

			c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
			c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.21f, 0.28f, 1.00f);
			c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.26f, 0.34f, 1.00f);

			c[ImGuiCol_Button] = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
			c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.38f, 0.50f, 1.00f);
			c[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.19f, 0.26f, 1.00f);

			c[ImGuiCol_Header] = ImVec4(0.18f, 0.24f, 0.32f, 0.80f);
			c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.38f, 0.50f, 0.90f);
			c[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.48f, 0.62f, 1.00f);

			c[ImGuiCol_CheckMark] = ImVec4(0.72f, 0.85f, 0.92f, 1.00f);
			c[ImGuiCol_SliderGrab] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_SliderGrabActive] = ImVec4(0.58f, 0.76f, 0.88f, 1.00f);

			c[ImGuiCol_Separator] = ImVec4(0.18f, 0.23f, 0.30f, 0.90f);
			c[ImGuiCol_SeparatorHovered] = ImVec4(0.42f, 0.62f, 0.74f, 0.78f);
			c[ImGuiCol_SeparatorActive] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);

			c[ImGuiCol_ResizeGrip] = ImVec4(0.42f, 0.62f, 0.74f, 0.18f);
			c[ImGuiCol_ResizeGripHovered] = ImVec4(0.42f, 0.62f, 0.74f, 0.60f);
			c[ImGuiCol_ResizeGripActive] = ImVec4(0.58f, 0.76f, 0.88f, 0.95f);

			c[ImGuiCol_Tab] = ImVec4(0.09f, 0.12f, 0.17f, 0.90f);
			c[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.38f, 0.50f, 1.00f);
			c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
			c[ImGuiCol_TabSelectedOverline] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_TabDimmed] = ImVec4(0.06f, 0.08f, 0.11f, 0.80f);
			c[ImGuiCol_TabDimmedSelected] = ImVec4(0.11f, 0.14f, 0.19f, 0.90f);
			c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.26f, 0.36f, 0.46f, 0.80f);

			c[ImGuiCol_DockingPreview] = ImVec4(0.42f, 0.62f, 0.74f, 0.45f);
			c[ImGuiCol_DockingEmptyBg] = ImVec4(0.04f, 0.05f, 0.08f, 1.00f);

			c[ImGuiCol_PlotLines] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_PlotLinesHovered] = ImVec4(0.58f, 0.76f, 0.88f, 1.00f);
			c[ImGuiCol_PlotHistogram] = ImVec4(0.32f, 0.52f, 0.68f, 1.00f);
			c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);

			c[ImGuiCol_TableHeaderBg] = ImVec4(0.09f, 0.12f, 0.17f, 1.00f);
			c[ImGuiCol_TableBorderStrong] = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
			c[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
			c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			c[ImGuiCol_TableRowBgAlt] = ImVec4(0.07f, 0.09f, 0.12f, 0.40f);

			c[ImGuiCol_NavCursor] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.42f, 0.62f, 0.74f, 0.70f);
			c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.04f, 0.20f);

			if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
			{
				style.WindowRounding = 0.0f;
				style.Colors[ImGuiCol_WindowBg].w = 1.0f;
			}
		}

		std::mutex* g_queueMutex = nullptr;
		PFN_vkQueueSubmit g_realVkQueueSubmit = nullptr;
		PFN_vkQueueWaitIdle g_realVkQueueWaitIdle = nullptr;
		PFN_vkQueuePresentKHR g_realVkQueuePresentKHR = nullptr;
		PFN_vkDeviceWaitIdle g_realVkDeviceWaitIdle = nullptr;

		VKAPI_ATTR VkResult VKAPI_CALL WrappedVkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence)
		{
			std::lock_guard lock(*g_queueMutex);
			return g_realVkQueueSubmit(queue, count, infos, fence);
		}

		VKAPI_ATTR VkResult VKAPI_CALL WrappedVkQueueWaitIdle(VkQueue queue)
		{
			std::lock_guard lock(*g_queueMutex);
			return g_realVkQueueWaitIdle(queue);
		}

		VKAPI_ATTR VkResult VKAPI_CALL WrappedVkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info)
		{
			std::lock_guard lock(*g_queueMutex);
			return g_realVkQueuePresentKHR(queue, info);
		}

		VKAPI_ATTR VkResult VKAPI_CALL WrappedVkDeviceWaitIdle(VkDevice device)
		{
			std::lock_guard lock(*g_queueMutex);
			return g_realVkDeviceWaitIdle(device);
		}
	} // namespace

	// ── ImVector helpers (no IM_FREE) ────────────────────────────────────────────

	// Zeros an ImVector so its destructor is a no-op. Used after memcpy-ing a
	// struct containing ImVectors to detach them from the source's memory.
	template<typename T>
	static void DetachImVector(ImVector<T>& vec)
	{
		vec.Size = 0;
		vec.Capacity = 0;
		vec.Data = nullptr;
	}

	// ── ImGuiSlot ----------------------------------------------------------------

	void ImGuiRenderer::ImGuiSlot::Free()
	{
		for (int i = 0; i < drawData.CmdLists.Size; i++)
		{
			ImDrawList* list = drawData.CmdLists[i];
			if (list)
			{
				list->~ImDrawList();
				ImGui::MemFree(list);
			}
		}
		drawData.CmdLists.clear();
		valid = false;
	}

	// ── Init / Shutdown ───────────────────────────────────────────────────────

	void ImGuiRenderer::Init(ServiceContainer& services, GLFWwindow* window)
	{
		AE_PROFILE_ZONE();
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		ApplyAetherTheme();
		ImGui_ImplGlfw_InitForVulkan(window, /*installCallbacks=*/true);

		const VulkanContext& ctx = services.Get<VulkanContext>();
		const VkDevice dev = ctx.GetDevice().device;

		g_queueMutex = &ctx.GetGraphicsQueueMutex();

		struct LoaderCtx
		{
			VkInstance instance;
			VkDevice device;
		};

		LoaderCtx loaderCtx{ ctx.GetInstance().instance, dev };
		ImGui_ImplVulkan_LoadFunctions(
		        VK_API_VERSION_1_3,
		        [](const char* fname, void* udata) -> PFN_vkVoidFunction
		        {
			        auto* c = static_cast<LoaderCtx*>(udata);
			        auto resolve = [&](const char* name) -> PFN_vkVoidFunction
			        {
				        PFN_vkVoidFunction fn = vkGetInstanceProcAddr(c->instance, name);
				        if (!fn)
				        {
					        fn = vkGetDeviceProcAddr(c->device, name);
				        }
				        return fn;
			        };

#	define INTERCEPT(vkName, realPtr, wrapper)                          \
    if (std::strcmp(fname, #vkName) == 0) {                          \
        realPtr = reinterpret_cast<decltype(realPtr)>(resolve(fname)); \
        return reinterpret_cast<PFN_vkVoidFunction>(wrapper);        \
    }
			        INTERCEPT(vkQueueSubmit, g_realVkQueueSubmit, WrappedVkQueueSubmit)
			        INTERCEPT(vkQueueWaitIdle, g_realVkQueueWaitIdle, WrappedVkQueueWaitIdle)
			        INTERCEPT(vkQueuePresentKHR, g_realVkQueuePresentKHR, WrappedVkQueuePresentKHR)
			        INTERCEPT(vkDeviceWaitIdle, g_realVkDeviceWaitIdle, WrappedVkDeviceWaitIdle)
#	undef INTERCEPT

			        return resolve(fname);
		        },
		        &loaderCtx);

		const VkFormat swapFmt = services.Get<Swapchain>().GetImageFormat();
		VkPipelineRenderingCreateInfoKHR pipelineRenderCI{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
		pipelineRenderCI.colorAttachmentCount = 1;
		pipelineRenderCI.pColorAttachmentFormats = &swapFmt;

		ImGui_ImplVulkan_InitInfo vulkanInfo{};
		vulkanInfo.ApiVersion = VK_API_VERSION_1_3;
		vulkanInfo.Instance = ctx.GetInstance().instance;
		vulkanInfo.PhysicalDevice = ctx.GetDevice().physical_device.physical_device;
		vulkanInfo.Device = dev;
		vulkanInfo.QueueFamily = ctx.GetGraphicsQueueFamily();
		vulkanInfo.Queue = ctx.GetGraphicsQueue();
		vulkanInfo.DescriptorPoolSize = 256;
		vulkanInfo.MinImageCount = Swapchain::kMaxFramesInFlight;
		vulkanInfo.ImageCount = Swapchain::kMaxFramesInFlight;
		vulkanInfo.UseDynamicRendering = true;
		vulkanInfo.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRenderCI;
		vulkanInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo = pipelineRenderCI;

		const bool imguiInitOk = ImGui_ImplVulkan_Init(&vulkanInfo);
		AE_ASSERT_ALWAYS(imguiInitOk, "ImGui_ImplVulkan_Init failed");

		RenderGraph& renderGraph = services.Get<RenderGraph>();
		auto swapColor = renderGraph.GetSwapchainColor();
		renderGraph.AddPass("ImGui")
		        .WriteColor(swapColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                const uint32_t slot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                RenderSlot(ctx, slot);
		                });
	}

	void ImGuiRenderer::Shutdown(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		if (vkDeviceWaitIdle(services.Get<VulkanContext>().GetDevice().device) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "ImGuiRenderer: failed to wait for device idle."));
		}
		for (auto& slot : m_slots)
		{
			slot.Free();
		}
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		m_services = nullptr;
	}

	// ── Game-thread API ───────────────────────────────────────────────────────

	void ImGuiRenderer::BeginFrame()
	{
		AE_PROFILE_ZONE();
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiRenderer::SnapshotFrame()
	{
		AE_PROFILE_ZONE();
		ImGui::Render();

		ImDrawData* src = ImGui::GetDrawData();
		auto& s = m_slots[m_writeSlot];

		s.Free();

		if (!src || !src->Valid || src->CmdListsCount == 0 ||
		    src->DisplaySize.x <= 0.f || src->DisplaySize.y <= 0.f)
		{
			return;
		}

		const int listCount = src->CmdListsCount;

		// Copy scalar ImDrawData fields.
		s.drawData.Valid = true;
		s.drawData.DisplayPos = src->DisplayPos;
		s.drawData.DisplaySize = src->DisplaySize;
		s.drawData.FramebufferScale = src->FramebufferScale;
		s.drawData.OwnerViewport = src->OwnerViewport;
		s.drawData.Textures = src->Textures;

		// Allocate the CmdLists pointer array using ImVector's own allocator.
		s.drawData.CmdLists.resize(listCount);

		int totalVtxCount = 0;
		int totalIdxCount = 0;

		for (int i = 0; i < listCount; ++i)
		{
			const ImDrawList* srcList = src->CmdLists[i];

			ImDrawList* dst = static_cast<ImDrawList*>(ImGui::MemAlloc(sizeof(ImDrawList)));
			std::memcpy(dst, srcList, sizeof(ImDrawList));

			// Detach ALL ImVector members from the memcpy'd source.
			// Only VtxBuffer/IdxBuffer/CmdBuffer are populated with our own data
			// below. The internal vectors (_Path, _ClipRectStack, etc.) are unused
			// during rendering — zero them so ~ImDrawList doesn't free ImGui memory.
			DetachImVector(dst->VtxBuffer);
			DetachImVector(dst->IdxBuffer);
			DetachImVector(dst->CmdBuffer);
			DetachImVector(dst->_Path);
			DetachImVector(dst->_ClipRectStack);
			DetachImVector(dst->_TextureStack);
			DetachImVector(dst->_CallbacksDataBuf);
			DetachImVector(dst->_Splitter._Channels);
			dst->_Splitter._Current = 0;
			dst->_Splitter._Count = 1;

			if (srcList->VtxBuffer.Size > 0)
			{
				const size_t bytes = static_cast<size_t>(srcList->VtxBuffer.Size) * sizeof(ImDrawVert);
				dst->VtxBuffer.Data = static_cast<ImDrawVert*>(ImGui::MemAlloc(bytes));
				std::memcpy(dst->VtxBuffer.Data, srcList->VtxBuffer.Data, bytes);
				dst->VtxBuffer.Size = srcList->VtxBuffer.Size;
				dst->VtxBuffer.Capacity = srcList->VtxBuffer.Size;
			}
			totalVtxCount += srcList->VtxBuffer.Size;

			if (srcList->IdxBuffer.Size > 0)
			{
				const size_t bytes = static_cast<size_t>(srcList->IdxBuffer.Size) * sizeof(ImDrawIdx);
				dst->IdxBuffer.Data = static_cast<ImDrawIdx*>(ImGui::MemAlloc(bytes));
				std::memcpy(dst->IdxBuffer.Data, srcList->IdxBuffer.Data, bytes);
				dst->IdxBuffer.Size = srcList->IdxBuffer.Size;
				dst->IdxBuffer.Capacity = srcList->IdxBuffer.Size;
			}
			totalIdxCount += srcList->IdxBuffer.Size;

			if (srcList->CmdBuffer.Size > 0)
			{
				const size_t bytes = static_cast<size_t>(srcList->CmdBuffer.Size) * sizeof(ImDrawCmd);
				dst->CmdBuffer.Data = static_cast<ImDrawCmd*>(ImGui::MemAlloc(bytes));
				std::memcpy(dst->CmdBuffer.Data, srcList->CmdBuffer.Data, bytes);
				dst->CmdBuffer.Size = srcList->CmdBuffer.Size;
				dst->CmdBuffer.Capacity = srcList->CmdBuffer.Size;
			}

			s.drawData.CmdLists[i] = dst;
		}

		s.drawData.CmdListsCount = listCount;
		s.drawData.TotalVtxCount = totalVtxCount;
		s.drawData.TotalIdxCount = totalIdxCount;
		s.valid = true;
	}

	void ImGuiRenderer::RenderPlatformWindows()
	{
		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	void ImGuiRenderer::ReregisterPass(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		RenderGraph& renderGraph = services.Get<RenderGraph>();
		auto swapColor = renderGraph.GetSwapchainColor();
		renderGraph.AddPass("ImGui")
		        .WriteColor(swapColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                const uint32_t slot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                RenderSlot(ctx, slot);
		                });
	}

	// ── Render thread ─────────────────────────────────────────────────────────

	void ImGuiRenderer::RenderSlot(PassContext& ctx, uint32_t slot)
	{
		auto& s = m_slots[slot];
		if (!s.valid)
		{
			return;
		}

		ImGui_ImplVulkan_RenderDrawData(&s.drawData, ctx.recorder.GetCommandBuffer());
	}

} // namespace aether

#endif // AETHER_IMGUI
