#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <imgui.h>
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/IUiOverlay.hpp"

namespace aether
{
	class GpuDevice;
	struct FrameTarget;
	class ImguiFrameData;
	class ImguiViewportRenderer;
	class ServiceContainer;
	class VulkanContext;

	namespace gpu
	{
		class CommandList;
	}

	// Owns Dear ImGui lifetime for engine/tooling UI. Implements IUiOverlay so
	class ImguiSubsystem : public IUiOverlay
	{
	public:
		ImguiSubsystem();
		~ImguiSubsystem() override;

		ImguiSubsystem(const ImguiSubsystem&) = delete;
		ImguiSubsystem& operator=(const ImguiSubsystem&) = delete;
		ImguiSubsystem(ImguiSubsystem&&) = delete;
		ImguiSubsystem& operator=(ImguiSubsystem&&) = delete;

		void Init(ServiceContainer& services) override;
		void Shutdown(ServiceContainer& services) override;

		void BeginFrame(ServiceContainer& services, float deltaTimeSeconds) override;

		// Producer-thread frame tail: render ImGui, then update GLFW platform
		void Render() override;
		void UpdatePlatformWindows() override;

		[[nodiscard]] std::unique_ptr<IUiOverlayFrameData> AcquireFrameData() override;
		void SnapshotFrame(IUiOverlayFrameData& outFrame) override;
		void RecycleFrameData(std::unique_ptr<IUiOverlayFrameData> frame) override;

		// Releases the game-thread frame lock taken in BeginFrame.  The producer
		void EndFrameLock() override;

		[[nodiscard]] std::vector<std::uint32_t> SecondaryViewportIdsWithPendingDestroy() const override;

		// SnapshotFrame would then vkQueueSubmit from the producer thread, which
		[[nodiscard]] bool HasPendingTextureUpdates() const override;

		void RenderFrame(const IUiOverlayFrameData& frame, gpu::CommandList& commands, const FrameTarget& target) override;

		// Render + present every secondary (torn-out) viewport (render thread).
		void RenderViewports(const IUiOverlayFrameData& frame) override;

		// Destroy render-thread swapchains for departed viewports (producer thread,
		void RetireViewports(const std::vector<std::uint32_t>& departedIds) override;

		// Enable/disable multi-viewport at runtime (producer thread).
		void SetViewportsEnabled(bool enabled) override;

		// Sets the manual editor UI-scale multiplier (producer thread).  Composes
		void SetUiScale(float uiScale) override;

		[[nodiscard]] ImTextureID RegisterTexture(gpu::ImageView imageView, gpu::ImageLayout layout);
		void UnregisterTexture(ImTextureID textureId);

		// retire frame.  Only safe when the GPU is idle and the render thread is
		void FlushPendingTextureReleasesImmediate() override;

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_initialized;
		}

		[[nodiscard]] bool WantsInputCapture() const noexcept override
		{
			return m_wantsInputCapture;
		}

		[[nodiscard]] float GetLastRenderCpuTimeMs() const noexcept
		{
			return m_lastRenderCpuTimeMs.load(std::memory_order_relaxed);
		}

	private:
		struct PendingTextureRelease
		{
			ImTextureID textureId = ImTextureID_Invalid;
			std::uint64_t retireFrame = 0;
		};

		void InitBackends(ServiceContainer& services);
		void ShutdownBackends();
		void RetirePendingTextureReleases();

		void ClampWindowsToMainViewport();

		bool m_initialized = false;
		bool m_backendsInitialized = false;
		bool m_wantsInputCapture = false;
		std::uint64_t m_frameIndex = 0;
		std::atomic<float> m_lastRenderCpuTimeMs = 0.0f;

		std::mutex m_mutex;
		std::optional<std::unique_lock<std::mutex>> m_gameThreadFrameLock;

		std::vector<std::byte> m_fontData;
		std::vector<std::byte> m_iconFontData;
		std::vector<std::byte> m_iconFontDataRegular;

		std::vector<PendingTextureRelease> m_pendingTextureReleases;
		std::unique_ptr<ImguiViewportRenderer> m_viewportRenderer;

		bool m_viewportsEnabled = true;
		int m_clampWindowsFrames = 0;
	};

} // namespace aether
