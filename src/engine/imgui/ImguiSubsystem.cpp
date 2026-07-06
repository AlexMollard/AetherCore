#include "imgui/ImguiSubsystem.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <span>

#include "Color.hpp"
#include "io/FileSystem.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/FrameTarget.hpp"
#include "imgui/ImguiFrameData.hpp"
#include "imgui/ImguiViewportRenderer.hpp"
#include "platform/Window.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/volk.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	namespace
	{
		void ApplyTheme()
		{
			auto& style = ImGui::GetStyle();
			auto& colors = style.Colors;

			using colors::Background, colors::Surface, colors::SurfaceElevated, colors::Border, colors::TextPrimary, colors::TextSecondary, colors::Orange, colors::Yellow, colors::Green, colors::Red;

			constexpr auto black = [](int a)
			{
				return ImColor(0, 0, 0, a);
			};

			constexpr auto withAlpha = [](const ImColor& c, int a)
			{
				return ImColor(c.Value.x, c.Value.y, c.Value.z, a / 255.0f);
			};

			const auto toIm = [](const glm::vec4& c)
			{
				return ImColor(c.r, c.g, c.b, c.a);
			};

			const auto bg = toIm(Background);
			const auto bgSurface = toIm(Surface);
			const auto bgElevated = toIm(SurfaceElevated);
			const auto cBorder = toIm(Border);
			const auto fg = toIm(TextPrimary);
			const auto fgMuted = toIm(TextSecondary);
			const auto cOrange = toIm(Orange);
			const auto cYellow = toIm(Yellow);
			const auto cGreen = toIm(Green);
			const auto cRed = toIm(Red);

			// Rounding and spacing
			style.FrameRounding = 0.0f;
			style.GrabRounding = 0.0f;
			style.ChildRounding = 0.0f;
			style.PopupRounding = 0.0f;
			style.TabRounding = 0.0f;
			style.WindowRounding = 0.0f;
			style.FrameBorderSize = 0.0f;
			style.WindowBorderSize = 1.0f;
			style.ChildBorderSize = 0.0f;
			style.PopupBorderSize = 1.0f;
			style.TabBorderSize = 0.0f;
			style.ScrollbarSize = 12.0f;
			style.WindowMenuButtonPosition = ImGuiDir_Right;
			style.ItemSpacing = ImVec2(10.0f, 3.0f);
			style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
			style.IndentSpacing = 10.0f;
			style.ScrollbarRounding = 0.0f;
			style.GrabMinSize = 5.0f;

			// Text
			colors[ImGuiCol_Text] = fg;
			colors[ImGuiCol_TextDisabled] = fgMuted;
			colors[ImGuiCol_TextLink] = cOrange;
			colors[ImGuiCol_TextSelectedBg] = withAlpha(cOrange, 48);

			// Window
			colors[ImGuiCol_WindowBg] = bg;
			colors[ImGuiCol_ChildBg] = bg;
			colors[ImGuiCol_PopupBg] = bgElevated; // elevated above surface
			colors[ImGuiCol_Border] = cBorder;
			colors[ImGuiCol_BorderShadow] = ImColor(0, 0, 0, 0);

			// Title
			colors[ImGuiCol_TitleBg] = bgSurface;
			colors[ImGuiCol_TitleBgActive] = bgElevated;
			colors[ImGuiCol_TitleBgCollapsed] = bg;

			// Menu
			colors[ImGuiCol_MenuBarBg] = bgSurface;

			// Scrollbar
			colors[ImGuiCol_ScrollbarBg] = bg;
			colors[ImGuiCol_ScrollbarGrab] = bgElevated;
			colors[ImGuiCol_ScrollbarGrabHovered] = withAlpha(cOrange, 140);
			colors[ImGuiCol_ScrollbarGrabActive] = cOrange;

			// Checkbox, radio, slider
			colors[ImGuiCol_CheckMark] = cGreen;
			colors[ImGuiCol_CheckboxSelectedBg] = withAlpha(cGreen, 40);
			colors[ImGuiCol_SliderGrab] = cOrange;
			colors[ImGuiCol_SliderGrabActive] = cYellow;

			// Button
			colors[ImGuiCol_Button] = bgSurface;
			colors[ImGuiCol_ButtonHovered] = withAlpha(cOrange, 180);
			colors[ImGuiCol_ButtonActive] = cOrange;

			// Header (collapsing headers, tree nodes, selectables)
			colors[ImGuiCol_Header] = bgSurface;
			colors[ImGuiCol_HeaderHovered] = withAlpha(cOrange, 100);
			colors[ImGuiCol_HeaderActive] = withAlpha(cOrange, 160);

			// Separator
			colors[ImGuiCol_Separator] = cBorder;
			colors[ImGuiCol_SeparatorHovered] = cOrange;
			colors[ImGuiCol_SeparatorActive] = cYellow;

			// Resize grip
			colors[ImGuiCol_ResizeGrip] = bgSurface;
			colors[ImGuiCol_ResizeGripHovered] = withAlpha(cOrange, 140);
			colors[ImGuiCol_ResizeGripActive] = cOrange;

			// Frame BG (input fields, combo, etc.)
			colors[ImGuiCol_FrameBg] = bgSurface;
			colors[ImGuiCol_FrameBgHovered] = withAlpha(cOrange, 60);
			colors[ImGuiCol_FrameBgActive] = withAlpha(cOrange, 100);

			// Input text
			colors[ImGuiCol_InputTextCursor] = fg;

			// Tabs
			colors[ImGuiCol_Tab] = bg;
			colors[ImGuiCol_TabHovered] = withAlpha(cOrange, 80);
			colors[ImGuiCol_TabSelected] = bgSurface;
			colors[ImGuiCol_TabSelectedOverline] = cOrange;
			colors[ImGuiCol_TabDimmed] = bg;
			colors[ImGuiCol_TabDimmedSelected] = bgSurface;
			colors[ImGuiCol_TabDimmedSelectedOverline] = withAlpha(cOrange, 80);

			// Docking
			colors[ImGuiCol_DockingPreview] = withAlpha(cOrange, 120);
			colors[ImGuiCol_DockingEmptyBg] = bg;

			// Plot
			colors[ImGuiCol_PlotLines] = fgMuted;
			colors[ImGuiCol_PlotLinesHovered] = cOrange;
			colors[ImGuiCol_PlotHistogram] = cOrange;
			colors[ImGuiCol_PlotHistogramHovered] = cYellow;

			// Tables
			colors[ImGuiCol_TableHeaderBg] = bgElevated;
			colors[ImGuiCol_TableBorderStrong] = cBorder;
			colors[ImGuiCol_TableBorderLight] = withAlpha(cBorder, 80);
			colors[ImGuiCol_TableRowBg] = bg;
			colors[ImGuiCol_TableRowBgAlt] = withAlpha(bgSurface, 160);

			// Tree
			colors[ImGuiCol_TreeLines] = cBorder;

			// Unsaved marker
			colors[ImGuiCol_UnsavedMarker] = cYellow;

			// Modal dimming
			colors[ImGuiCol_ModalWindowDimBg] = black(128);

			// Drag and drop
			colors[ImGuiCol_DragDropTarget] = cOrange;
			colors[ImGuiCol_DragDropTargetBg] = withAlpha(cOrange, 48);

			// Nav
			colors[ImGuiCol_NavCursor] = withAlpha(cOrange, 100);
			colors[ImGuiCol_NavWindowingHighlight] = withAlpha(fg, 112);
			colors[ImGuiCol_NavWindowingDimBg] = black(128);
		}
	} // anonymous namespace

	ImguiSubsystem::~ImguiSubsystem()
	{
		if (m_initialized)
		{
			ShutdownBackends();
			ImGui::DestroyContext();
		}
	}

	void ImguiSubsystem::Init(ServiceContainer& services)
	{
		if (m_initialized)
		{
			return;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();

		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		// Install multi-viewport support unconditionally so the runtime toggle is just a
		// flag flip; SettingsService::ApplyAll applies the persisted on/off at startup.
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		// ImGui 1.92 per-monitor DPI: dynamic fonts re-rasterize crisply at monitor DPI
		// (ConfigDpiScaleFonts -> FontScaleDpi), and ImGui window geometry scales across
		// monitors (ConfigDpiScaleViewports). Complementary; no double font scaling. The
		// framebuffer-size DisplaySize override is intentionally dropped so imgui_impl_glfw
		// owns the DPI-aware DisplaySize (logical) + DisplayFramebufferScale.
		io.ConfigDpiScaleFonts = true;
		io.ConfigDpiScaleViewports = true;

		ApplyTheme();

		// Multi-viewport: OS windows must be opaque or they render translucent.
		ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;

		// Load Roboto Regular for tooling UI.
		constexpr std::string_view kFontPath = "assets://fonts/Roboto-Regular.ttf";
		if (io::FileSystem::Exists(kFontPath))
		{
			auto result = io::FileSystem::ReadFile(kFontPath);
			if (result)
			{
				m_fontData = std::move(*result);
				ImFontConfig fontConfig{};
				fontConfig.FontDataOwnedByAtlas = false;
				fontConfig.SizePixels = 15.0f;
				fontConfig.OversampleH = 3;
				fontConfig.OversampleV = 3;
				fontConfig.PixelSnapH = false;
				io.Fonts->AddFontFromMemoryTTF(m_fontData.data(), static_cast<int>(m_fontData.size()), fontConfig.SizePixels, &fontConfig);
				io.FontDefault = io.Fonts->Fonts.back();
			}
		}

		// Merge Font Awesome 6 Free-Solid into the same atlas: icon codepoints sit
		// in the Private Use Area (0xE000-0xF8FF) so text rendering stays Roboto.
		// Consumers use the ICON_FA_* UTF-8 literals (app: debug/Icons.hpp).
		constexpr std::string_view kIconFontPath = "assets://fonts/fa-solid-900.ttf";
		if (!io.Fonts->Fonts.empty() && io::FileSystem::Exists(kIconFontPath))
		{
			auto result = io::FileSystem::ReadFile(kIconFontPath);
			if (result)
			{
				m_iconFontData = std::move(*result);
				static const ImWchar kIconRange[] = {0xE000, 0xF8FF, 0};
				ImFontConfig iconConfig{};
				iconConfig.FontDataOwnedByAtlas = false;
				iconConfig.MergeMode = true;
				iconConfig.PixelSnapH = true;
				iconConfig.SizePixels = 13.0f;
				iconConfig.GlyphMinAdvanceX = 16.0f;
				iconConfig.GlyphOffset = ImVec2(0.0f, 1.0f);
				io.Fonts->AddFontFromMemoryTTF(m_iconFontData.data(), static_cast<int>(m_iconFontData.size()), iconConfig.SizePixels, &iconConfig, kIconRange);
			}
		}

		m_initialized = true;
		InitBackends(services);
		AE_INFO(LogCategory::UI, "Dear ImGui subsystem initialized.");
	}

	void ImguiSubsystem::Shutdown(ServiceContainer&)
	{
		if (!m_initialized)
		{
			return;
		}

		m_gameThreadFrameLock.reset();
		ShutdownBackends();
		ImGui::DestroyContext();
		m_initialized = false;
		m_frameIndex = 0;
		m_pendingTextureReleases.clear();
		AE_INFO(LogCategory::UI, "Dear ImGui subsystem shutdown.");
	}

	void ImguiSubsystem::BeginFrame(ServiceContainer& services, float deltaTimeSeconds)
	{
		if (!m_initialized)
		{
			return;
		}

		m_gameThreadFrameLock.reset();
		m_gameThreadFrameLock.emplace(m_mutex);
		RetirePendingTextureReleases();
		ImGuiIO& io = ImGui::GetIO();
		// imgui_impl_glfw's NewFrame sets the DPI-aware DisplaySize (logical) +
		// DisplayFramebufferScale each frame; only provide a fallback when the backends
		// (and thus that NewFrame) aren't running, so ImGui::NewFrame never sees size 0.
		if (!m_backendsInitialized)
		{
			if (auto window = services.TryGet<Window>())
			{
				const auto extent = window->GetFramebufferSize();
				io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
			}
		}
		io.DeltaTime = deltaTimeSeconds > 0.0f ? deltaTimeSeconds : 1.0f / 60.0f;

		if (m_backendsInitialized)
		{
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplGlfw_NewFrame();
		}
		ImGui::NewFrame();
		++m_frameIndex;
	}

	void ImguiSubsystem::Render()
	{
		if (!m_initialized)
		{
			return;
		}
		ImGui::Render();
		const ImGuiIO& io = ImGui::GetIO();
		m_wantsInputCapture = io.WantCaptureMouse || io.WantCaptureKeyboard;
	}

	void ImguiSubsystem::UpdatePlatformWindows()
	{
		if (!m_initialized)
		{
			return;
		}
		// When ViewportsEnable is set, ImGui REQUIRES UpdatePlatformWindows() every frame
		// after Render() (it asserts on the next NewFrame otherwise). GLFW-only here: the
		// Vulkan renderer hooks are nulled, so spawning an OS window touches no GPU state,
		// and the swapchain/backend/pipeline all already exist from Init.
		if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::UpdatePlatformWindows();
		}
	}

	std::vector<ImGuiID> ImguiSubsystem::SecondaryViewportIdsWithPendingDestroy() const
	{
		std::vector<ImGuiID> departed;
		if (!m_initialized)
		{
			return departed;
		}
		const ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
		for (ImGuiViewportP* vp: g.Viewports)
		{
			if (vp == mainViewport)
			{
				continue;
			}
			// A created OS window that was not active this frame will be destroyed by the
			// next UpdatePlatformWindows(); its render-thread swapchain must retire first.
			if (vp->PlatformWindowCreated && vp->LastFrameActive < g.FrameCount)
			{
				departed.push_back(vp->ID);
			}
		}
		return departed;
	}

	void ImguiSubsystem::SnapshotFrame(ImguiFrameData& outFrame)
	{
		if (!m_initialized)
		{
			return;
		}
		// The game thread already holds m_mutex via m_gameThreadFrameLock (BeginFrame), so
		// do NOT re-lock here.
		outFrame.Capture(ImGui::GetDrawData()); // main viewport
		if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
			for (ImGuiViewport* vp: platformIO.Viewports)
			{
				if (vp == mainViewport || vp->DrawData == nullptr || vp->PlatformHandle == nullptr)
				{
					continue;
				}
				// Per-viewport framebuffer scale (correct on hi-DPI secondary monitors);
				// keeps the swapchain size EnsureWindow picks consistent with the render
				// viewport in RenderOne.
				outFrame.CaptureSecondary(vp->DrawData, vp->ID, vp->Pos, vp->Size, vp->DrawData->FramebufferScale, vp->PlatformHandle);
			}
		}
	}

	void ImguiSubsystem::EndFrameLock()
	{
		m_gameThreadFrameLock.reset();
	}

	void ImguiSubsystem::RenderViewports(const ImguiFrameData& frame)
	{
		if (!m_initialized || m_viewportRenderer == nullptr)
		{
			return;
		}
		m_viewportRenderer->Render(frame);
	}

	void ImguiSubsystem::RetireViewports(const std::vector<ImGuiID>& departedIds)
	{
		if (m_viewportRenderer != nullptr)
		{
			m_viewportRenderer->RetireViewports(departedIds);
		}
	}

	void ImguiSubsystem::SetViewportsEnabled(bool enabled)
	{
		if (!m_initialized)
		{
			return;
		}
		m_viewportsEnabled = enabled;
		ImGuiIO& io = ImGui::GetIO();
		if (enabled)
		{
			io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		}
		else
		{
			io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
		}
	}

	void ImguiSubsystem::SetUiScale(float uiScale)
	{
		if (!m_initialized)
		{
			return;
		}
		ImGui::GetStyle().FontScaleMain = std::clamp(uiScale, 0.5f, 3.0f);
	}

	void ImguiSubsystem::RenderFrame(const ImguiFrameData& frame, gpu::CommandList& commands, const FrameTarget& target)
	{
		if (!m_initialized || !m_backendsInitialized || !frame.HasDrawData())
		{
			m_lastRenderCpuTimeMs.store(0.0f, std::memory_order_relaxed);
			return;
		}

		std::lock_guard lock(m_mutex);

		const gpu::RenderingAttachmentInfo colorAttachment{
		        .imageView = target.colorView,
		        .imageLayout = gpu::ImageLayout::ColorAttachment,
		        .loadOp = gpu::LoadOp::Load,
		        .storeOp = gpu::StoreOp::Store,
		};
		const gpu::RenderingInfo renderInfo{
		        .width = target.extent.width,
		        .height = target.extent.height,
		        .layerCount = 1,
		        .colorAttachments = std::span<const gpu::RenderingAttachmentInfo>(&colorAttachment, 1),
		};

		commands.BeginDebugLabel("ImGui", 0.95f, 0.45f, 0.15f, 1.0f);
		commands.BeginRendering(renderInfo);
		const auto t0 = std::chrono::high_resolution_clock::now();
		ImGui_ImplVulkan_RenderDrawData(const_cast<ImDrawData*>(frame.GetDrawData()), reinterpret_cast<VkCommandBuffer>(commands.GetCommandBuffer()));
		const auto t1 = std::chrono::high_resolution_clock::now();
		m_lastRenderCpuTimeMs.store(std::chrono::duration<float, std::milli>(t1 - t0).count(), std::memory_order_relaxed);
		commands.EndRendering();
		commands.EndDebugLabel();
	}

	ImTextureID ImguiSubsystem::RegisterTexture(gpu::ImageView imageView, gpu::ImageLayout layout)
	{
		if (!m_initialized || !m_backendsInitialized || imageView == nullptr)
		{
			return ImTextureID_Invalid;
		}

		const VkDescriptorSet descriptorSet = ImGui_ImplVulkan_AddTexture(static_cast<VkImageView>(imageView), ToVk(layout));
		return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(descriptorSet));
	}

	void ImguiSubsystem::UnregisterTexture(ImTextureID textureId)
	{
		if (!m_initialized || !m_backendsInitialized || textureId == ImTextureID_Invalid)
		{
			return;
		}

		constexpr std::uint64_t kTextureReleaseDelayFrames = kMaxFramesInFlight + 1u;
		m_pendingTextureReleases.push_back(PendingTextureRelease{
		        .textureId = textureId,
		        .retireFrame = m_frameIndex + kTextureReleaseDelayFrames,
		});
	}

	void ImguiSubsystem::FlushPendingTextureReleasesImmediate()
	{
		if (!m_backendsInitialized)
		{
			m_pendingTextureReleases.clear();
			return;
		}

		for (const auto& pending: m_pendingTextureReleases)
		{
			ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(pending.textureId)));
		}
		m_pendingTextureReleases.clear();
	}

	void ImguiSubsystem::RetirePendingTextureReleases()
	{
		if (!m_backendsInitialized)
		{
			m_pendingTextureReleases.clear();
			return;
		}

		for (auto it = m_pendingTextureReleases.begin(); it != m_pendingTextureReleases.end();)
		{
			if (it->retireFrame <= m_frameIndex)
			{
				ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(it->textureId)));
				it = m_pendingTextureReleases.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void ImguiSubsystem::InitBackends(ServiceContainer& services)
	{
		if (m_backendsInitialized)
		{
			return;
		}

		auto& vk = services.Get<VulkanContext>();
		auto& window = services.Get<Window>();
		auto& swapchain = services.Get<Swapchain>();

		ImGui_ImplGlfw_InitForVulkan(window.GetHandle(), true);

		VkPipelineRenderingCreateInfoKHR renderingInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
		        .colorAttachmentCount = 1,
		};
		const VkFormat colorFormat = ToVk(swapchain.GetImageFormat());
		renderingInfo.pColorAttachmentFormats = &colorFormat;

		ImGui_ImplVulkan_InitInfo initInfo{};
		initInfo.ApiVersion = VK_API_VERSION_1_4;
		initInfo.Instance = vk.GetInstance().instance;
		initInfo.PhysicalDevice = vk.GetPhysicalDevice();
		initInfo.Device = vk.GetDevice().device;
		initInfo.QueueFamily = vk.GetGraphicsQueueFamily();
		initInfo.Queue = vk.GetGraphicsQueue();
		initInfo.DescriptorPoolSize = 4096;
		initInfo.MinImageCount = 2;
		initInfo.ImageCount = Swapchain::kMaxFramesInFlight;
		initInfo.PipelineCache = vk.GetPipelineCache();
		initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
		initInfo.UseDynamicRendering = true;
		initInfo.MinAllocationSize = 1024 * 1024;
		initInfo.CheckVkResultFn = [](VkResult err)
		{
			if (err != VK_SUCCESS)
			{
				AE_ERROR(LogCategory::Vulkan, "ImGui Vulkan backend error: {}", static_cast<int>(err));
			}
		};

		if (!ImGui_ImplVulkan_Init(&initInfo))
		{
			AE_WARN(LogCategory::UI, "Dear ImGui Vulkan backend initialization failed.");
			ImGui_ImplGlfw_Shutdown();
			return;
		}

		// Suppress the stock Vulkan renderer viewport hooks: UpdatePlatformWindows() must do
		// GLFW-only work (no swapchain / vkDeviceWaitIdle on the producer thread) and we
		// never call RenderPlatformWindowsDefault(). Secondary viewports render on the
		// render thread via our own ImguiViewportRenderer. The glfw PLATFORM hooks stay.
		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Renderer_CreateWindow = nullptr;
		platformIO.Renderer_DestroyWindow = nullptr;
		platformIO.Renderer_SetWindowSize = nullptr;
		platformIO.Renderer_RenderWindow = nullptr;
		platformIO.Renderer_SwapBuffers = nullptr;

		m_viewportRenderer = std::make_unique<ImguiViewportRenderer>();
		m_viewportRenderer->Init(vk, ToVk(swapchain.GetImageFormat()));

		m_backendsInitialized = true;
	}

	void ImguiSubsystem::ShutdownBackends()
	{
		if (!m_backendsInitialized)
		{
			return;
		}

		for (const PendingTextureRelease& pending: m_pendingTextureReleases)
		{
			ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(pending.textureId)));
		}
		m_pendingTextureReleases.clear();
		if (m_viewportRenderer)
		{
			m_viewportRenderer->Shutdown();
			m_viewportRenderer.reset();
		}
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		m_backendsInitialized = false;
	}
} // namespace aether
