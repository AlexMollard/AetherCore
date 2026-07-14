#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace aether
{
	class ServiceContainer;
	struct FrameTarget;

	namespace gpu
	{
		class CommandList;
	}

	// never look inside this - only the concrete overlay that produced it (via
	class IUiOverlayFrameData
	{
	public:
		virtual ~IUiOverlayFrameData() = default;
	};

	// never does, so AetherCore's overlay pointer stays null there: it
	class IUiOverlay
	{
	public:
		virtual ~IUiOverlay() = default;

		virtual void Init(ServiceContainer& services) = 0;
		virtual void Shutdown(ServiceContainer& services) = 0;

		// Producer/game-thread frame lifecycle - mirrors ImguiSubsystem's API.
		virtual void BeginFrame(ServiceContainer& services, float deltaTimeSeconds) = 0;
		virtual void Render() = 0;
		virtual void UpdatePlatformWindows() = 0;
		virtual void EndFrameLock() = 0;

		[[nodiscard]] virtual std::unique_ptr<IUiOverlayFrameData> AcquireFrameData() = 0;
		virtual void SnapshotFrame(IUiOverlayFrameData& outFrame) = 0;
		virtual void RecycleFrameData(std::unique_ptr<IUiOverlayFrameData> frame) = 0;

		[[nodiscard]] virtual std::vector<std::uint32_t> SecondaryViewportIdsWithPendingDestroy() const = 0;
		virtual void RetireViewports(const std::vector<std::uint32_t>& departedIds) = 0;

		// producer thread; vkQueueSubmit is externally synchronized, so AetherCore
		[[nodiscard]] virtual bool HasPendingTextureUpdates() const = 0;

		// Render thread: draw the captured frame into the current target, then
		virtual void RenderFrame(const IUiOverlayFrameData& frame, gpu::CommandList& commands, const FrameTarget& target) = 0;
		virtual void RenderViewports(const IUiOverlayFrameData& frame) = 0;

		virtual void SetViewportsEnabled(bool enabled) = 0;
		virtual void SetUiScale(float uiScale) = 0;
		virtual void FlushPendingTextureReleasesImmediate() = 0;

		[[nodiscard]] virtual bool WantsInputCapture() const = 0;
	};

} // namespace aether
