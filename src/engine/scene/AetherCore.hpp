#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "ServiceContainer.hpp"
#include "animation/AnimationDatabase.hpp"
#include "animation/ModelAnimator.hpp"
#include "assets/AssetSubsystem.hpp"
#include "material/Material.hpp"
#include "camera/CameraSubsystem.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "rendering/CommandRecorder.hpp"
#include "rendering/RenderThread.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/SceneSubsystem.hpp"
#include "ui/UISubsystem.hpp"
#include "utils/EngineSettings.hpp"
#include "vulkan/GraphicsDevice.hpp"
#include "vulkan/Swapchain.hpp"

namespace aether
{
	struct LoadedModelPrimitive
	{
		Mesh mesh;
		Material material{};
		glm::mat4 localTransform{ 1.0f };
		std::int32_t skinIndex = -1;
	};

	struct LoadedModel
	{
		std::vector<Texture> textures;
		std::vector<LoadedModelPrimitive> primitives;
		std::optional<ModelAnimator> animator;
		AnimationDatabase animationDb;
	};

	class AetherCore
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			int width = 1280;
			int height = 720;
			bool enableVsync = true;
			const char* settingsFile = "engine.toml";
			const char* uiFontPath = "";
			const char* uiPassNamePrefix = "UIPass";
			int uiGlyphSize = 48;
		};

		struct CameraRenderTarget
		{
			uint32_t id = 0;

			[[nodiscard]] bool IsValid() const
			{
				return id != 0;
			}
		};

		explicit AetherCore(const Config& config = {});
		AetherCore(const Config& config, const EngineSettings& settings);
		~AetherCore();

		[[nodiscard]] ServiceContainer& GetServiceContainer()
		{
			return m_services;
		}

		// Frame lifecycle.
		[[nodiscard]] bool ShouldClose();
		void PumpEvents();
		void Tick(float dt);
		[[nodiscard]] RenderFramePacket PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex);
		void ExecuteRenderFrame(const RenderFramePacket& packet);
		void WaitIdle();

		[[nodiscard]] static constexpr VkFormat GetForwardColorFormat()
		{
			return PostProcessStack::GetForwardColorFormat();
		}

		void SetSwapchainRecreatedCallback(std::function<void(AetherCore&)> cb)
		{
			m_swapchainRecreatedCallback = std::move(cb);
		}

	private:
		void BeginFrame();
		void EndFrame(const RenderFramePacket& packet);
		void RecreateSwapchain();

		struct AsyncComputeFrame
		{
			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkFence inFlight = VK_NULL_HANDLE;
		};

		ServiceContainer m_services;
		PlatformSubsystem m_platform;
		GraphicsDevice m_gfx;
		SceneSubsystem m_sceneSub;
		AssetSubsystem m_assetsSub;
		CameraSubsystem m_cameras;
		UISubsystem m_ui;
		RenderingSubsystem m_rendering;

		// Frame-lifecycle state still owned by AetherCore.
		std::array<AsyncComputeFrame, Swapchain::kMaxFramesInFlight> m_asyncComputeFrames{};
		VkSemaphore m_computeTimelineSemaphore = VK_NULL_HANDLE;
		std::uint64_t m_computeTimelineValue = 0;
		bool m_asyncComputeEnabled = false;
		CommandRecorder m_currentRecorder;
		std::uint64_t m_frameIndex = 0;
		EngineSettings m_settings{};
		std::function<void(AetherCore&)> m_swapchainRecreatedCallback;
	};
} // namespace aether
