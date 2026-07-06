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

	// Owns Dear ImGui lifetime for engine/tooling UI.
	class ImguiSubsystem
	{
	public:
		ImguiSubsystem() = default;
		~ImguiSubsystem();

		ImguiSubsystem(const ImguiSubsystem&) = delete;
		ImguiSubsystem& operator=(const ImguiSubsystem&) = delete;
		ImguiSubsystem(ImguiSubsystem&&) = delete;
		ImguiSubsystem& operator=(ImguiSubsystem&&) = delete;

		void Init(ServiceContainer& services);
		void Shutdown(ServiceContainer& services);

		void BeginFrame(ServiceContainer& services, float deltaTimeSeconds);
		// Producer-thread frame tail (replaces the old CaptureFrame): render ImGui, then
		// update GLFW platform windows (secondary viewports), then snapshot main + every
		// secondary viewport for the render thread.
		void Render();
		void UpdatePlatformWindows();
		void SnapshotFrame(ImguiFrameData& outFrame);
		// Releases the game-thread frame lock taken in BeginFrame. The producer calls this
		// before a viewport-destroy RunExclusive quiesce so the render thread can drain/park
		// without deadlocking on the ImGui mutex this thread holds.
		void EndFrameLock();
		// Secondary viewports whose OS window will be destroyed this frame (retire under a
		// quiesce before UpdatePlatformWindows destroys the GLFW window).
		[[nodiscard]] std::vector<ImGuiID> SecondaryViewportIdsWithPendingDestroy() const;
		void RenderFrame(const ImguiFrameData& frame, gpu::CommandList& commands, const FrameTarget& target);
		// Render + present every secondary (torn-out) viewport (render thread).
		void RenderViewports(const ImguiFrameData& frame);
		// Destroy render-thread swapchains for departed viewports (producer thread, only
		// inside a RunExclusive quiesce).
		void RetireViewports(const std::vector<ImGuiID>& departedIds);
		// Enable/disable multi-viewport at runtime (producer thread).
		void SetViewportsEnabled(bool enabled);
		[[nodiscard]] ImTextureID RegisterTexture(gpu::ImageView imageView, gpu::ImageLayout layout);
		void UnregisterTexture(ImTextureID textureId);

		// Free every queued ImGui descriptor immediately, ignoring the deferred
		// retire frame. Only safe to call when the GPU is idle and the render
		// thread is parked (i.e. inside the quiesced swapchain/viewport recreate),
		// so no in-flight command buffer still references the descriptors.
		void FlushPendingTextureReleasesImmediate();

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_initialized;
		}

		[[nodiscard]] bool WantsInputCapture() const noexcept
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

		bool m_initialized = false;
		bool m_backendsInitialized = false;
		bool m_wantsInputCapture = false;
		std::uint64_t m_frameIndex = 0;
		std::atomic<float> m_lastRenderCpuTimeMs = 0.0f;
		std::mutex m_mutex;
		std::optional<std::unique_lock<std::mutex>> m_gameThreadFrameLock;
		std::vector<std::byte> m_fontData;
		std::vector<std::byte> m_iconFontData;
		std::vector<PendingTextureRelease> m_pendingTextureReleases;
		std::unique_ptr<ImguiViewportRenderer> m_viewportRenderer;
		bool m_viewportsEnabled = true;
	};
} // namespace aether
