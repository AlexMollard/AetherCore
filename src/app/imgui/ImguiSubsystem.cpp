#include "imgui/ImguiSubsystem.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include "imgui/UiAutomation.hpp"
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
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
#include "debug/EditorChrome.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
	namespace
	{

		void ApplyTheme()
		{
			auto& style = ImGui::GetStyle();

			style.FrameRounding = 3.0f;
			style.GrabRounding = 3.0f;
			style.ChildRounding = 3.0f;
			style.PopupRounding = 3.0f;
			style.TabRounding = 3.0f;
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

			editor::chrome::RefreshTokens();
			editor::chrome::ApplyImGuiColors(style);
		}

		void ProcessBackendTextureUpdates(ImDrawData* drawData)
		{
			if (drawData == nullptr || drawData->Textures == nullptr)
			{
				return;
			}

			for (ImTextureData* texture: *drawData->Textures)
			{
				if (texture != nullptr && texture->Status != ImTextureStatus_OK)
				{
					ImGui_ImplVulkan_UpdateTexture(texture);
				}
			}
		}

	} // namespace

	// Lifetime

	ImguiSubsystem::ImguiSubsystem() = default;

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
		// Route item-info hooks (IMGUI_ENABLE_TEST_ENGINE) to the UI-automation registry.
		if (ImGuiContext* g = ImGui::GetCurrentContext())
		{
			g->TestEngineHookItems = true;
		}
		ImGuiIO& io = ImGui::GetIO();

		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		io.ConfigDpiScaleFonts = true;
		io.ConfigDpiScaleViewports = true;

		io.ConfigWindowsMoveFromTitleBarOnly = true;

		ApplyTheme();

		// Multi-viewport: OS windows must be opaque or they render translucent.
		ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 1.0f;

		constexpr std::string_view kFontPath = "engine://fonts/Roboto-Regular.ttf";
		if (io::FileSystem::Exists(kFontPath))
		{
			auto result = io::FileSystem::ReadFile(kFontPath);
			if (result)
			{
				m_fontData = std::move(*result);
				ImFontConfig fontConfig{};
				fontConfig.FontDataOwnedByAtlas = false;
				io.Fonts->AddFontFromMemoryTTF(m_fontData.data(), static_cast<int>(m_fontData.size()), 15.0f, &fontConfig);
				io.FontDefault = io.Fonts->Fonts.back();
			}
		}

		constexpr std::string_view kIconFontPath = "engine://fonts/fa-solid-900.ttf";
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
				iconConfig.SizePixels = 15.0f;
				iconConfig.GlyphMinAdvanceX = 15.0f;
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

	// Frame lifecycle (producer / game thread)

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

		if (!m_backendsInitialized)
		{
			if (auto* window = services.TryGet<Window>())
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

		// Publish last frame's item registry and inject synthetic input after the
		// GLFW backend has posted real events (so injected events win this frame).
		app::UiAutomation::Get().BeginFrameSwap();
		app::UiAutomation::Get().ApplyInput(io);

		ImGui::NewFrame();
		++m_frameIndex;

		if (m_clampWindowsFrames > 0)
		{
			ClampWindowsToMainViewport();
			--m_clampWindowsFrames;
		}
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

		if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			ImGui::UpdatePlatformWindows();
		}
	}

	std::vector<std::uint32_t> ImguiSubsystem::SecondaryViewportIdsWithPendingDestroy() const
	{
		std::vector<std::uint32_t> departed;
		if (!m_initialized)
		{
			return departed;
		}

		const ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiViewport* mainViewport = ImGui::GetMainViewport();

		for (const ImGuiViewportP* vp: g.Viewports)
		{
			if (vp == mainViewport)
			{
				continue;
			}

			if (vp->PlatformWindowCreated && vp->LastFrameActive < g.FrameCount)
			{
				departed.push_back(vp->ID);
			}
		}
		return departed;
	}

	std::unique_ptr<IUiOverlayFrameData> ImguiSubsystem::AcquireFrameData()
	{
		return ImguiFrameData::AcquirePooled();
	}

	void ImguiSubsystem::SnapshotFrame(IUiOverlayFrameData& outFrame)
	{
		if (!m_initialized)
		{
			return;
		}

		auto& frame = dynamic_cast<ImguiFrameData&>(outFrame);

		// The game thread already holds m_mutex via m_gameThreadFrameLock
		ImDrawData* mainDrawData = ImGui::GetDrawData();
		ProcessBackendTextureUpdates(mainDrawData);
		frame.Capture(mainDrawData);

		if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			const ImGuiViewport* mainViewport = ImGui::GetMainViewport();

			for (const ImGuiViewport* vp: platformIO.Viewports)
			{
				if (vp == mainViewport || vp->DrawData == nullptr || vp->PlatformHandle == nullptr)
				{
					continue;
				}

				ProcessBackendTextureUpdates(vp->DrawData);
				frame.CaptureSecondary(vp->DrawData, vp->ID, vp->Pos, vp->Size, vp->DrawData->FramebufferScale, vp->PlatformHandle);
			}
		}
	}

	bool ImguiSubsystem::HasPendingTextureUpdates() const
	{
		if (!m_initialized)
		{
			return false;
		}

		// ImGui_ImplVulkan_UpdateTexture (a producer-thread vkQueueSubmit) makes
		const auto drawDataHasPending = [](const ImDrawData* drawData)
		{
			if (drawData == nullptr || drawData->Textures == nullptr)
			{
				return false;
			}
			for (const ImTextureData* texture: *drawData->Textures)
			{
				if (texture != nullptr && texture->Status != ImTextureStatus_OK)
				{
					return true;
				}
			}
			return false;
		};

		if (drawDataHasPending(ImGui::GetDrawData()))
		{
			return true;
		}

		if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
		{
			const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
			for (const ImGuiViewport* vp: platformIO.Viewports)
			{
				if (vp == mainViewport || vp->PlatformHandle == nullptr)
				{
					continue;
				}
				if (drawDataHasPending(vp->DrawData))
				{
					return true;
				}
			}
		}
		return false;
	}

	void ImguiSubsystem::RecycleFrameData(std::unique_ptr<IUiOverlayFrameData> frame)
	{
		ImguiFrameData::RecyclePooled(std::unique_ptr<ImguiFrameData>(dynamic_cast<ImguiFrameData*>(frame.release())));
	}

	void ImguiSubsystem::EndFrameLock()
	{
		m_gameThreadFrameLock.reset();
	}

	void ImguiSubsystem::RenderViewports(const IUiOverlayFrameData& frame)
	{
		if (!m_initialized || m_viewportRenderer == nullptr)
		{
			return;
		}

		m_viewportRenderer->Render(dynamic_cast<const ImguiFrameData&>(frame));
	}

	void ImguiSubsystem::RetireViewports(const std::vector<std::uint32_t>& departedIds)
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
			m_clampWindowsFrames = 3;
		}
	}

	void ImguiSubsystem::ClampWindowsToMainViewport()
	{
		if (!m_initialized)
		{
			return;
		}

		const ImGuiContext& g = *ImGui::GetCurrentContext();
		const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
		const ImVec2 workMin = mainViewport->WorkPos;
		const ImVec2 workMax = ImVec2(mainViewport->WorkPos.x + mainViewport->WorkSize.x, mainViewport->WorkPos.y + mainViewport->WorkSize.y);
		const float titleH = ImGui::GetFrameHeight();

		for (ImGuiWindow* window: g.Windows)
		{
			if (window == nullptr || !window->WasActive || (window->Flags & ImGuiWindowFlags_ChildWindow) != 0)
			{
				continue;
			}

			ImVec2 pos = window->Pos;
			pos.x = std::clamp(pos.x, workMin.x, std::max(workMin.x, workMax.x - window->Size.x));
			pos.y = std::clamp(pos.y, workMin.y, std::max(workMin.y, workMax.y - titleH));

			if (pos.x != window->Pos.x || pos.y != window->Pos.y)
			{
				ImGui::SetWindowPos(window, pos, ImGuiCond_Always);
			}
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

	// Render thread

	void ImguiSubsystem::RenderFrame(const IUiOverlayFrameData& overlayFrame, gpu::CommandList& commands, const FrameTarget& target)
	{
		const auto& frame = dynamic_cast<const ImguiFrameData&>(overlayFrame);
		if (!m_initialized || !m_backendsInitialized || !frame.HasDrawData())
		{
			m_lastRenderCpuTimeMs.store(0.0f, std::memory_order_relaxed);
			return;
		}

		const std::lock_guard lock(m_mutex);

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
		ImGui_ImplVulkan_RenderDrawData(const_cast<ImDrawData*>(frame.GetDrawData()), reinterpret_cast<VkCommandBuffer>(commands.GetCommandBuffer())); // NOLINT(cppcoreguidelines-pro-type-const-cast): Dear ImGui's backend API is not const-correct.
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
		initInfo.MinAllocationSize = static_cast<VkDeviceSize>(1024 * 1024);
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

		// must do GLFW-only work and we never call RenderPlatformWindowsDefault().
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
