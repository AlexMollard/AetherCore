#pragma once

#include <cstdint>
#include <memory>

#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether
{
	struct FrameConstants;

	class AnimationBlendSystem;
	class GpuDevice;
	class CameraSubsystem;
	class ImguiSubsystem;
	class RenderingSubsystem;

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

		[[nodiscard]] static GpuFormat GetForwardColorFormat();

		[[nodiscard]] std::vector<DebugVertex>& GetPendingDebugVertices()
		{
			return m_pendingDebugVertices;
		}

	private:
		void BeginFrame();
		void EndFrame(const RenderFramePacket& packet);
		void RecreateSwapchain();

		[[nodiscard]] std::vector<std::string> GetRenderPassNames() const;
		[[nodiscard]] std::size_t GetRenderPassCount() const;

		void BuildShadowsAndRunLighting(const RenderFramePacket& packet, std::uint32_t frameIdx, FrameConstants& fc);
		void PatchShadowIndices(std::uint32_t frameIdx);
		void UploadFrameConstantsAndExecuteRenderGraph(std::uint32_t frameIdx, const FrameConstants& fc);
		void SubmitAndAdvance();

		ServiceContainer m_services;
		std::vector<DebugVertex> m_pendingDebugVertices;

		std::unique_ptr<GpuDevice> m_gpu;
		std::unique_ptr<CameraSubsystem> m_cameras;
		std::unique_ptr<ImguiSubsystem> m_imgui;
		std::unique_ptr<RenderingSubsystem> m_rendering;

		std::unique_ptr<AnimationBlendSystem> m_animationBlend;

		gpu::CommandList m_currentCmdList;
		std::uint64_t m_frameIndex = 0;

		EngineSettings m_settings{};
	};
} // namespace aether
