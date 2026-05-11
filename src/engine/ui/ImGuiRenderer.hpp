#pragma once

#ifdef AETHER_IMGUI

#	include <array>
#	include <cstdint>

#	include <imgui.h>

#	include "vulkan/Swapchain.hpp"

struct GLFWwindow;

namespace aether
{
	class AetherCore;
	struct PassContext;

	// Integrates Dear ImGui (docking branch) with AetherCore's render graph.
	//
	// Threading model:
	//   Game thread : BeginFrame() -> [layer OnGui calls] -> SnapshotFrame()
	//                 -> RenderPlatformWindows() (secondary OS windows)
	//   Render thread : registered RenderGraph pass calls RenderSlot()
	//
	// SnapshotFrame stores a pointer to ImGui's draw data (no deep-copy).
	// Safety is guaranteed by the frame-slot lifecycle: the game thread cannot
	// call ImGui::NewFrame() (which invalidates the data) until the render
	// thread has finished consuming that slot, enforced by SubmitFrame sync.
	class ImGuiRenderer
	{
	public:
		void Init(AetherCore& engine, GLFWwindow* window);
		void Shutdown(AetherCore& engine);

		void SetWriteSlot(uint32_t slot)
		{
			m_writeSlot = slot;
		}

		// Game thread - before any OnGui callbacks.
		void BeginFrame();

		// Game thread - after all OnGui callbacks.
		// Finalises the ImGui frame and records a pointer to the draw data.
		void SnapshotFrame();

		// Game thread - after SnapshotFrame, before SubmitFrame.
		// Renders secondary OS windows (ViewportsEnable) using imgui_impl_vulkan.
		void RenderPlatformWindows();

		// Called from the swapchain-recreated callback to re-register the render
		// graph pass (the graph is cleared on every swapchain resize).
		void ReregisterPass(AetherCore& engine);

	private:
		struct FrameSlot
		{
			// Borrowed from ImGui - valid until the next ImGui::NewFrame().
			ImDrawData* drawData = nullptr;
			bool valid = false;
		};

		void RenderSlot(PassContext& ctx, uint32_t slot);

		std::array<FrameSlot, Swapchain::kMaxFramesInFlight> m_slots;
		uint32_t m_writeSlot = 0;

		AetherCore* m_engine = nullptr;
	};

} // namespace aether

#else // AETHER_IMGUI not defined - no-op stub, zero ImGui dependency

#	include <cstdint>

struct GLFWwindow;

namespace aether
{
	class AetherCore;

	class ImGuiRenderer
	{
	public:
		void Init(AetherCore&, GLFWwindow*)
		{
		}

		void Shutdown(AetherCore&)
		{
		}

		void SetWriteSlot(uint32_t)
		{
		}

		void BeginFrame()
		{
		}

		void SnapshotFrame()
		{
		}

		void RenderPlatformWindows()
		{
		}

		void ReregisterPass(AetherCore&)
		{
		}
	};

} // namespace aether

#endif // AETHER_IMGUI
