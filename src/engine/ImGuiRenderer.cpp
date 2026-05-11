#include "ImGuiRenderer.hpp"

#ifdef AETHER_IMGUI

#	include <cstring>
#	include <mutex>

#	include <backends/imgui_impl_glfw.h>
#	include <backends/imgui_impl_vulkan.h>
#	include <GLFW/glfw3.h>

#	include "AetherCore.hpp"
#	include "RenderGraph.hpp"
#	include "Swapchain.hpp"
#	include "VulkanContext.hpp"

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
	//                              ImGui_ImplVulkanH_DestroyWindow (first viewport
	//                              open / close), imgui_impl_vulkan.cpp:1667, 1915
	//
	//   Render thread (inside RenderDrawData → UpdateTexture, first frame only):
	//     vkQueueSubmit          - font-atlas upload
	//     vkQueueWaitIdle        - wait for font-atlas upload, imgui_impl_vulkan.cpp:922
	//
	// AetherCore::EndFrame wraps m_swapchain.EndFrame() with g_queueMutex.
	// These wrappers intercept every imgui queue op and lock the same mutex so
	// all accesses to the VkQueue serialise correctly.
	namespace
	{
		// ── AetherCore ImGui theme ─────────────────────────────────────────────────
		// Palette is aligned with UiTheme.hpp so both UI systems feel cohesive.
		// Deep space navy backgrounds, electric-steel accent, cool off-white text.
		static void ApplyAetherTheme()
		{
			ImGuiStyle& style = ImGui::GetStyle();

			// ── Shape ──────────────────────────────────────────────────────────────
			style.WindowRounding = 8.0f; // Doesnt do anything with multiu-viewports enabled
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

			// ── Spacing ────────────────────────────────────────────────────────────
			style.WindowPadding = ImVec2(10.0f, 8.0f);
			style.FramePadding = ImVec2(6.0f, 4.0f);
			style.CellPadding = ImVec2(6.0f, 4.0f);
			style.ItemSpacing = ImVec2(8.0f, 6.0f);
			style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
			style.IndentSpacing = 20.0f;
			style.ScrollbarSize = 12.0f;
			style.GrabMinSize = 10.0f;

			// ── Colours ────────────────────────────────────────────────────────────
			ImVec4* c = style.Colors;

			// Backgrounds - deep midnight navy
			c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.08f, 0.11f, 0.97f);
			c[ImGuiCol_ChildBg] = ImVec4(0.04f, 0.06f, 0.09f, 0.95f);
			c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.09f, 0.13f, 0.98f);
			c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.03f, 0.55f);

			// Borders
			c[ImGuiCol_Border] = ImVec4(0.18f, 0.23f, 0.30f, 0.80f);
			c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

			// Text
			c[ImGuiCol_Text] = ImVec4(0.88f, 0.91f, 0.93f, 1.00f);
			c[ImGuiCol_TextDisabled] = ImVec4(0.38f, 0.46f, 0.54f, 1.00f);
			c[ImGuiCol_TextSelectedBg] = ImVec4(0.42f, 0.62f, 0.74f, 0.35f);

			// Title bar
			c[ImGuiCol_TitleBg] = ImVec4(0.07f, 0.09f, 0.13f, 1.00f);
			c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.13f, 0.18f, 1.00f);
			c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.05f, 0.06f, 0.09f, 0.80f);

			// Menu bar
			c[ImGuiCol_MenuBarBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);

			// Scrollbar
			c[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.05f, 0.08f, 0.70f);
			c[ImGuiCol_ScrollbarGrab] = ImVec4(0.20f, 0.26f, 0.34f, 1.00f);
			c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.28f, 0.36f, 0.46f, 1.00f);
			c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);

			// Frame (input fields, sliders, checkboxes)
			c[ImGuiCol_FrameBg] = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
			c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.21f, 0.28f, 1.00f);
			c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.26f, 0.34f, 1.00f);

			// Buttons - steel blue family
			c[ImGuiCol_Button] = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
			c[ImGuiCol_ButtonHovered] = ImVec4(0.28f, 0.38f, 0.50f, 1.00f);
			c[ImGuiCol_ButtonActive] = ImVec4(0.14f, 0.19f, 0.26f, 1.00f);

			// Header (collapsibles, selectables, table rows)
			c[ImGuiCol_Header] = ImVec4(0.18f, 0.24f, 0.32f, 0.80f);
			c[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.38f, 0.50f, 0.90f);
			c[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.48f, 0.62f, 1.00f);

			// Checkmark / slider grab / grab active - cyan accent
			c[ImGuiCol_CheckMark] = ImVec4(0.72f, 0.85f, 0.92f, 1.00f);
			c[ImGuiCol_SliderGrab] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_SliderGrabActive] = ImVec4(0.58f, 0.76f, 0.88f, 1.00f);

			// Separator
			c[ImGuiCol_Separator] = ImVec4(0.18f, 0.23f, 0.30f, 0.90f);
			c[ImGuiCol_SeparatorHovered] = ImVec4(0.42f, 0.62f, 0.74f, 0.78f);
			c[ImGuiCol_SeparatorActive] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);

			// Resize grip
			c[ImGuiCol_ResizeGrip] = ImVec4(0.42f, 0.62f, 0.74f, 0.18f);
			c[ImGuiCol_ResizeGripHovered] = ImVec4(0.42f, 0.62f, 0.74f, 0.60f);
			c[ImGuiCol_ResizeGripActive] = ImVec4(0.58f, 0.76f, 0.88f, 0.95f);

			// Tabs
			c[ImGuiCol_Tab] = ImVec4(0.09f, 0.12f, 0.17f, 0.90f);
			c[ImGuiCol_TabHovered] = ImVec4(0.28f, 0.38f, 0.50f, 1.00f);
			c[ImGuiCol_TabSelected] = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
			c[ImGuiCol_TabSelectedOverline] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_TabDimmed] = ImVec4(0.06f, 0.08f, 0.11f, 0.80f);
			c[ImGuiCol_TabDimmedSelected] = ImVec4(0.11f, 0.14f, 0.19f, 0.90f);
			c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.26f, 0.36f, 0.46f, 0.80f);

			// Docking
			c[ImGuiCol_DockingPreview] = ImVec4(0.42f, 0.62f, 0.74f, 0.45f);
			c[ImGuiCol_DockingEmptyBg] = ImVec4(0.04f, 0.05f, 0.08f, 1.00f);

			// Plot
			c[ImGuiCol_PlotLines] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_PlotLinesHovered] = ImVec4(0.58f, 0.76f, 0.88f, 1.00f);
			c[ImGuiCol_PlotHistogram] = ImVec4(0.32f, 0.52f, 0.68f, 1.00f);
			c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);

			// Table
			c[ImGuiCol_TableHeaderBg] = ImVec4(0.09f, 0.12f, 0.17f, 1.00f);
			c[ImGuiCol_TableBorderStrong] = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
			c[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
			c[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
			c[ImGuiCol_TableRowBgAlt] = ImVec4(0.07f, 0.09f, 0.12f, 0.40f);

			// Navigation cursor
			c[ImGuiCol_NavCursor] = ImVec4(0.42f, 0.62f, 0.74f, 1.00f);
			c[ImGuiCol_NavWindowingHighlight] = ImVec4(0.42f, 0.62f, 0.74f, 0.70f);
			c[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.04f, 0.20f);

			// Multi-viewport OS windows need square corners and opaque bg to avoid
			// a visible seam between the OS window border and the rounded inner frame.
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

	// ── Init / Shutdown ───────────────────────────────────────────────────────

	void ImGuiRenderer::Init(AetherCore& engine, GLFWwindow* window)
	{
		m_engine = &engine;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		// ViewportsEnable (separate OS windows): vkQueueSubmit from the game
		// thread (RenderPlatformWindowsDefault) is serialized with the render
		// thread's EndFrame submit via g_queueMutex / WrappedVkQueueSubmit.
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		ApplyAetherTheme();
		ImGui_ImplGlfw_InitForVulkan(window, /*installCallbacks=*/true);

		// Save stable context pointers used on the render thread.
		// Both are valid until ImGui::DestroyContext().
		m_sharedData = ImGui::GetDrawListSharedData();
		m_mainViewport = ImGui::GetMainViewport();

		const VulkanContext& ctx = engine.GetVulkanContext();
		const VkDevice dev = ctx.GetDevice().device;

		// Point the module-level mutex pointer at VulkanContext's mutex so that
		// WrappedVkQueueSubmit can lock it without capturing anything.
		g_queueMutex = &ctx.GetGraphicsQueueMutex();

		// With VK_NO_PROTOTYPES, imgui_impl_vulkan cannot call Vulkan directly.
		// Supply volk's already-resolved function pointers via the loader callback.
		// LoadFunctions() invokes the callback immediately to populate an internal
		// dispatch table; passing a stack-allocated LoaderCtx is safe.
		// We also intercept vkQueueSubmit here to inject our mutex wrapper.
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

		// Dynamic rendering setup (no VkRenderPass needed).
		const VkFormat swapFmt = engine.GetSwapchainImageFormat();
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
		// DescriptorPoolSize > 0 tells imgui_impl_vulkan to create and own its pool.
		vulkanInfo.DescriptorPoolSize = 256;
		vulkanInfo.MinImageCount = Swapchain::kMaxFramesInFlight;
		vulkanInfo.ImageCount = Swapchain::kMaxFramesInFlight;
		vulkanInfo.UseDynamicRendering = true;
		// Pipeline info for main viewport (and secondary viewports share the same format).
		vulkanInfo.PipelineInfoMain.PipelineRenderingCreateInfo = pipelineRenderCI;
		vulkanInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo = pipelineRenderCI;

		ImGui_ImplVulkan_Init(&vulkanInfo);
		// Font texture upload is handled automatically by imgui_impl_vulkan 1.92+
		// (ImGuiBackendFlags_RendererHasTextures - no manual CreateFontsTexture needed).

		// Register the ImGui pass - composites on top of all other passes.
		auto swapColor = engine.GetRenderGraph().GetSwapchainColor();
		engine.GetRenderGraph()
		        .AddPass("ImGui")
		        .WriteColor(swapColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                const uint32_t slot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
			                RenderSlot(ctx, slot);
		                });
	}

	void ImGuiRenderer::Shutdown(AetherCore& engine)
	{
		engine.WaitIdle();

		for (auto& slot: m_slots)
		{
			slot.tempLists.clear();
		}

		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();

		m_sharedData = nullptr;
		m_engine = nullptr;
	}

	// ── Game-thread API ───────────────────────────────────────────────────────

	void ImGuiRenderer::BeginFrame()
	{
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
	}

	void ImGuiRenderer::SnapshotFrame()
	{
		ImGui::Render();

		const ImDrawData* src = ImGui::GetDrawData();
		auto& s = m_slots[m_writeSlot];
		s.lists.clear();
		s.hasData = false;

		if (!src || !src->Valid || src->CmdListsCount == 0 || src->DisplaySize.x <= 0.f || src->DisplaySize.y <= 0.f)
		{
			return;
		}

		s.displayPos = src->DisplayPos;
		s.displaySize = src->DisplaySize;
		s.fbScale = src->FramebufferScale;
		// Textures is a stable pointer into ImGui::GetPlatformIO().Textures - valid
		// for the lifetime of the context. RenderDrawData iterates it to upload any
		// pending ImTextureData (e.g. the font atlas on first frame).
		s.textures = src->Textures;

		// Deep-copy each draw list so the render thread can safely read them
		// after the game thread has called ImGui::NewFrame() for the next frame.
		s.lists.resize(src->CmdListsCount);
		for (int i = 0; i < src->CmdListsCount; ++i)
		{
			const ImDrawList* srcList = src->CmdLists[i];
			ListCopy& lc = s.lists[i];
			lc.vtx = srcList->VtxBuffer;
			lc.idx = srcList->IdxBuffer;
			lc.cmds = srcList->CmdBuffer;
			lc.flags = srcList->Flags;
		}

		s.hasData = true;
	}

	void ImGuiRenderer::RenderPlatformWindows()
	{
		// Render secondary OS windows (floating panels torn off the dock).
		// Called on the game thread immediately after SnapshotFrame so draw data
		// is still valid (NewFrame has not been called yet for the next frame).
		// vkQueueSubmit is serialized with the render thread via WrappedVkQueueSubmit.
		if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
		{
			ImGui::UpdatePlatformWindows();
			ImGui::RenderPlatformWindowsDefault();
		}
	}

	void ImGuiRenderer::ReregisterPass(AetherCore& engine)
	{
		auto swapColor = engine.GetRenderGraph().GetSwapchainColor();
		engine.GetRenderGraph()
		        .AddPass("ImGui")
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
		if (!s.hasData)
		{
			return;
		}

		const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

		// Reconstruct temporary ImDrawList objects from the deep-copied data.
		// imgui_impl_vulkan reads VtxBuffer / IdxBuffer / CmdBuffer from each list.
		s.tempLists.clear();
		s.tempLists.reserve(s.lists.size());

		ImDrawData drawData{};
		drawData.Valid = true;
		drawData.DisplayPos = s.displayPos;
		drawData.DisplaySize = s.displaySize;
		drawData.FramebufferScale = s.fbScale;
		// imgui_impl_vulkan dereferences OwnerViewport to find per-viewport render buffers.
		drawData.OwnerViewport = m_mainViewport;
		// imgui_impl_vulkan iterates Textures to upload any pending ImTextureData
		// (font atlas on first frame, dynamic atlas updates thereafter).
		drawData.Textures = s.textures;

		for (const ListCopy& lc: s.lists)
		{
			auto& tl = s.tempLists.emplace_back(std::make_unique<ImDrawList>(m_sharedData));
			tl->VtxBuffer = lc.vtx;
			tl->IdxBuffer = lc.idx;
			tl->CmdBuffer = lc.cmds;
			tl->Flags = lc.flags;

			drawData.CmdLists.push_back(tl.get());
			drawData.CmdListsCount++;
			drawData.TotalVtxCount += lc.vtx.Size;
			drawData.TotalIdxCount += lc.idx.Size;
		}

		ImGui_ImplVulkan_RenderDrawData(&drawData, cmd);
	}

} // namespace aether

#endif // AETHER_IMGUI
