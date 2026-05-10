#include "ImGuiRenderer.hpp"

#include <cstring>
#include <mutex>

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

#include "AetherCore.hpp"
#include "RenderGraph.hpp"
#include "Swapchain.hpp"
#include "VulkanContext.hpp"

namespace aether
{

// ── vkQueueSubmit wrapper ──────────────────────────────────────────────────
// imgui_impl_vulkan calls vkQueueSubmit on the graphics queue from the game
// thread (RenderPlatformWindowsDefault → RenderWindow for secondary viewports).
// AetherCore::EndFrame calls it from the render thread.  Serialize both via
// the VulkanContext mutex so the Vulkan spec's external-sync requirement is met.
namespace
{
    std::mutex*              g_queueMutex        = nullptr;
    PFN_vkQueueSubmit        g_realVkQueueSubmit = nullptr;

    VKAPI_ATTR VkResult VKAPI_CALL WrappedVkQueueSubmit(
        VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence)
    {
        std::lock_guard lock(*g_queueMutex);
        return g_realVkQueueSubmit(queue, count, infos, fence);
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

        ImGui::StyleColorsDark();
        ImGui_ImplGlfw_InitForVulkan(window, /*installCallbacks=*/true);

        // Save stable context pointers used on the render thread.
        // Both are valid until ImGui::DestroyContext().
        m_sharedData   = ImGui::GetDrawListSharedData();
        m_mainViewport = ImGui::GetMainViewport();

        const VulkanContext& ctx = engine.GetVulkanContext();
        const VkDevice       dev = ctx.GetDevice().device;

        // Point the module-level mutex pointer at VulkanContext's mutex so that
        // WrappedVkQueueSubmit can lock it without capturing anything.
        g_queueMutex = &ctx.GetGraphicsQueueMutex();

        // With VK_NO_PROTOTYPES, imgui_impl_vulkan cannot call Vulkan directly.
        // Supply volk's already-resolved function pointers via the loader callback.
        // LoadFunctions() invokes the callback immediately to populate an internal
        // dispatch table; passing a stack-allocated LoaderCtx is safe.
        // We also intercept vkQueueSubmit here to inject our mutex wrapper.
        struct LoaderCtx { VkInstance instance; VkDevice device; };
        LoaderCtx loaderCtx{
            ctx.GetInstance().instance,
            dev
        };
        ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3,
            [](const char* fname, void* udata) -> PFN_vkVoidFunction
            {
                auto* c = static_cast<LoaderCtx*>(udata);
                if (std::strcmp(fname, "vkQueueSubmit") == 0)
                {
                    // Save the real pointer so the wrapper can call through.
                    g_realVkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
                        vkGetDeviceProcAddr(c->device, fname));
                    if (!g_realVkQueueSubmit)
                        g_realVkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
                            vkGetInstanceProcAddr(c->instance, fname));
                    return reinterpret_cast<PFN_vkVoidFunction>(WrappedVkQueueSubmit);
                }
                PFN_vkVoidFunction fn = vkGetDeviceProcAddr(c->device, fname);
                if (!fn) fn = vkGetInstanceProcAddr(c->instance, fname);
                return fn;
            }, &loaderCtx);

        // Dynamic rendering setup (no VkRenderPass needed).
        const VkFormat swapFmt = engine.GetSwapchainImageFormat();
        VkPipelineRenderingCreateInfoKHR pipelineRenderCI{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
        pipelineRenderCI.colorAttachmentCount    = 1;
        pipelineRenderCI.pColorAttachmentFormats = &swapFmt;

        ImGui_ImplVulkan_InitInfo vulkanInfo{};
        vulkanInfo.ApiVersion            = VK_API_VERSION_1_3;
        vulkanInfo.Instance              = ctx.GetInstance().instance;
        vulkanInfo.PhysicalDevice        = ctx.GetDevice().physical_device.physical_device;
        vulkanInfo.Device                = dev;
        vulkanInfo.QueueFamily           = ctx.GetGraphicsQueueFamily();
        vulkanInfo.Queue                 = ctx.GetGraphicsQueue();
        // DescriptorPoolSize > 0 tells imgui_impl_vulkan to create and own its pool.
        vulkanInfo.DescriptorPoolSize    = 256;
        vulkanInfo.MinImageCount         = Swapchain::kMaxFramesInFlight;
        vulkanInfo.ImageCount            = Swapchain::kMaxFramesInFlight;
        vulkanInfo.UseDynamicRendering   = true;
        // Pipeline info for main viewport (and secondary viewports share the same format).
        vulkanInfo.PipelineInfoMain.PipelineRenderingCreateInfo      = pipelineRenderCI;
        vulkanInfo.PipelineInfoForViewports.PipelineRenderingCreateInfo = pipelineRenderCI;

        ImGui_ImplVulkan_Init(&vulkanInfo);
        // Font texture upload is handled automatically by imgui_impl_vulkan 1.92+
        // (ImGuiBackendFlags_RendererHasTextures - no manual CreateFontsTexture needed).

        // Register the ImGui pass - composites on top of all other passes.
        auto swapColor = engine.GetRenderGraph().GetSwapchainColor();
        engine.GetRenderGraph()
            .AddPass("ImGui")
            .WriteColor(swapColor, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
            .Execute([this](PassContext& ctx)
            {
                const uint32_t slot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
                RenderSlot(ctx, slot);
            });
    }

    void ImGuiRenderer::Shutdown(AetherCore& engine)
    {
        engine.WaitIdle();

        for (auto& slot : m_slots)
            slot.tempLists.clear();

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        m_sharedData = nullptr;
        m_engine     = nullptr;
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

        if (!src || !src->Valid || src->CmdListsCount == 0 ||
            src->DisplaySize.x <= 0.f || src->DisplaySize.y <= 0.f)
        {
            return;
        }

        s.displayPos  = src->DisplayPos;
        s.displaySize = src->DisplaySize;
        s.fbScale     = src->FramebufferScale;
        // Textures is a stable pointer into ImGui::GetPlatformIO().Textures - valid
        // for the lifetime of the context. RenderDrawData iterates it to upload any
        // pending ImTextureData (e.g. the font atlas on first frame).
        s.textures    = src->Textures;

        // Deep-copy each draw list so the render thread can safely read them
        // after the game thread has called ImGui::NewFrame() for the next frame.
        s.lists.resize(src->CmdListsCount);
        for (int i = 0; i < src->CmdListsCount; ++i)
        {
            const ImDrawList* srcList = src->CmdLists[i];
            ListCopy& lc = s.lists[i];
            lc.vtx   = srcList->VtxBuffer;
            lc.idx   = srcList->IdxBuffer;
            lc.cmds  = srcList->CmdBuffer;
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
            .Execute([this](PassContext& ctx)
            {
                const uint32_t slot = ctx.frameIndex % Swapchain::kMaxFramesInFlight;
                RenderSlot(ctx, slot);
            });
    }

    // ── Render thread ─────────────────────────────────────────────────────────

    void ImGuiRenderer::RenderSlot(PassContext& ctx, uint32_t slot)
    {
        auto& s = m_slots[slot];
        if (!s.hasData) return;

        const VkCommandBuffer cmd = ctx.recorder.GetCommandBuffer();

        // Reconstruct temporary ImDrawList objects from the deep-copied data.
        // imgui_impl_vulkan reads VtxBuffer / IdxBuffer / CmdBuffer from each list.
        s.tempLists.clear();
        s.tempLists.reserve(s.lists.size());

        ImDrawData drawData{};
        drawData.Valid            = true;
        drawData.DisplayPos       = s.displayPos;
        drawData.DisplaySize      = s.displaySize;
        drawData.FramebufferScale = s.fbScale;
        // imgui_impl_vulkan dereferences OwnerViewport to find per-viewport render buffers.
        drawData.OwnerViewport    = m_mainViewport;
        // imgui_impl_vulkan iterates Textures to upload any pending ImTextureData
        // (font atlas on first frame, dynamic atlas updates thereafter).
        drawData.Textures         = s.textures;

        for (const ListCopy& lc : s.lists)
        {
            auto& tl = s.tempLists.emplace_back(
                std::make_unique<ImDrawList>(m_sharedData));
            tl->VtxBuffer = lc.vtx;
            tl->IdxBuffer = lc.idx;
            tl->CmdBuffer = lc.cmds;
            tl->Flags     = lc.flags;

            drawData.CmdLists.push_back(tl.get());
            drawData.CmdListsCount++;
            drawData.TotalVtxCount += lc.vtx.Size;
            drawData.TotalIdxCount += lc.idx.Size;
        }

        ImGui_ImplVulkan_RenderDrawData(&drawData, cmd);
    }

} // namespace aether
