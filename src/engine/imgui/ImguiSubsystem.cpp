#include "imgui/ImguiSubsystem.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <span>

#include "gpu/GpuDevice.hpp"
#include "gpu/FrameTarget.hpp"
#include "imgui/ImguiFrameData.hpp"
#include "platform/Window.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/volk.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace aether
{
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

		if (auto window = services.TryGet<Window>())
		{
			const auto extent = window->GetFramebufferSize();
			io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
		}

		ImGui::StyleColorsDark();

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

		ShutdownBackends();
		ImGui::DestroyContext();
		m_initialized = false;
		m_frameIndex = 0;
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
		ImGuiIO& io = ImGui::GetIO();
		if (auto window = services.TryGet<Window>())
		{
			const auto extent = window->GetFramebufferSize();
			io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
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

	void ImguiSubsystem::CaptureFrame(ImguiFrameData& outFrame)
	{
		if (!m_initialized)
		{
			return;
		}

		ImGui::Render();
		outFrame.Capture(ImGui::GetDrawData());
		m_gameThreadFrameLock.reset();
	}

	void ImguiSubsystem::RenderFrame(const ImguiFrameData& frame, gpu::CommandList& commands, const FrameTarget& target)
	{
		if (!m_initialized || !m_backendsInitialized || !frame.HasDrawData())
		{
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
		ImGui_ImplVulkan_RenderDrawData(const_cast<ImDrawData*>(frame.GetDrawData()), reinterpret_cast<VkCommandBuffer>(commands.GetCommandBuffer()));
		commands.EndRendering();
		commands.EndDebugLabel();
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
		initInfo.DescriptorPoolSize = 64;
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

		m_backendsInitialized = true;
	}

	void ImguiSubsystem::ShutdownBackends()
	{
		if (!m_backendsInitialized)
		{
			return;
		}

		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		m_backendsInitialized = false;
	}
} // namespace aether
