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

	// Opaque per-frame snapshot produced by an IUiOverlay implementation (e.g.
	// Dear ImGui's captured draw-list data). AetherCore and RenderFramePacket
	// never look inside this - only the concrete overlay that produced it (via
	// RenderFrame/RenderViewports/RecycleFrameData) knows the real type.
	//
	// This is what lets the engine core's frame-packet pipeline stay free of
	// any UI-toolkit dependency: the shipped GameRuntime never constructs an
	// IUiOverlay, so every IUiOverlayFrameData pointer in the system stays
	// null there and no UI-toolkit code is ever linked or invoked.
	class IUiOverlayFrameData
	{
	public:
		virtual ~IUiOverlayFrameData() = default;
	};

	// Optional per-frame UI overlay extension point owned by AetherCore. The
	// engine core has zero knowledge of any concrete UI toolkit (Dear ImGui);
	// only an editor build (App, under AETHERCORE_EDITOR_APP) constructs a
	// concrete implementation (ImguiSubsystem, src/app/imgui/) and hands it to
	// AetherCore::SetUiOverlay after construction. The shipped GameRuntime
	// never does, so AetherCore's overlay pointer stays null there: it
	// neither links nor initializes any UI-toolkit code (see the null guards
	// around every call site in AetherCore.cpp).
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

		// Per-frame draw-data capture. Implementations are expected to pool
		// these to avoid per-frame heap churn (see ImguiFrameData::AcquirePooled).
		[[nodiscard]] virtual std::unique_ptr<IUiOverlayFrameData> AcquireFrameData() = 0;
		virtual void SnapshotFrame(IUiOverlayFrameData& outFrame) = 0;
		virtual void RecycleFrameData(std::unique_ptr<IUiOverlayFrameData> frame) = 0;

		// Secondary (torn-out) viewport lifecycle. IDs are opaque
		// overlay-defined handles (Dear ImGui's ImGuiID under the hood).
		[[nodiscard]] virtual std::vector<std::uint32_t> SecondaryViewportIdsWithPendingDestroy() const = 0;
		virtual void RetireViewports(const std::vector<std::uint32_t>& departedIds) = 0;

		// True when SnapshotFrame would perform backend texture uploads (create/
		// update/destroy). Texture uploads submit to the graphics queue from the
		// producer thread; vkQueueSubmit is externally synchronized, so AetherCore
		// must quiesce the render thread (RunExclusive) before a SnapshotFrame that
		// returns true here, or the two threads race on the queue (spec-level UB;
		// crashes inside the validation layer's state tracking).
		[[nodiscard]] virtual bool HasPendingTextureUpdates() const = 0;

		// Render thread: draw the captured frame into the current target, then
		// present every secondary viewport.
		virtual void RenderFrame(const IUiOverlayFrameData& frame, gpu::CommandList& commands, const FrameTarget& target) = 0;
		virtual void RenderViewports(const IUiOverlayFrameData& frame) = 0;

		virtual void SetViewportsEnabled(bool enabled) = 0;
		virtual void SetUiScale(float uiScale) = 0;
		virtual void FlushPendingTextureReleasesImmediate() = 0;

		[[nodiscard]] virtual bool WantsInputCapture() const = 0;
	};

} // namespace aether
