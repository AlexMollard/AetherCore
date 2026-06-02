#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "utils/ServiceContainer.hpp"
#include "animation/AnimationDatabase.hpp"
#include "assets/AssetSubsystem.hpp"
#include "camera/CameraSubsystem.hpp"
#include "gpu/AsyncComputeContext.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuTypes.hpp"
#include "material/Material.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "rendering/CommandRecorder.hpp"
#include "rendering/RenderThread.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/SceneSubsystem.hpp"
#include "ui/UISubsystem.hpp"
#include "utils/EngineSettings.hpp"

namespace aether
{
	struct FrameConstants;
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

		explicit AetherCore(const Config& config);
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

		[[nodiscard]] static constexpr GpuFormat GetForwardColorFormat()
		{
			return GpuDevice::GetForwardColorFormat();
		}

	private:
		void BeginFrame();
		void EndFrame(const RenderFramePacket& packet);
		void RecreateSwapchain();

		void BuildShadowsAndRunLighting(const RenderFramePacket& packet, std::uint32_t frameIdx, FrameConstants& fc);
		void PatchShadowIndices(std::uint32_t frameIdx);
		void UploadFrameConstantsAndExecuteRenderGraph(std::uint32_t frameIdx, const FrameConstants& fc);
		void SubmitAndAdvance();

		ServiceContainer m_services;
		PlatformSubsystem m_platform;
		GpuDevice m_gpu;
		AsyncComputeContext m_asyncCompute;
		AsyncComputeContext::SubmitResult m_asyncComputeSubmitResult{};
		SceneSubsystem m_sceneSub;
		AssetSubsystem m_assetsSub;
		CameraSubsystem m_cameras;
		UISubsystem m_ui;
		RenderingSubsystem m_rendering;

		CommandRecorder m_currentRecorder;
		std::uint64_t m_frameIndex = 0;
		EngineSettings m_settings{};
	};
} // namespace aether
