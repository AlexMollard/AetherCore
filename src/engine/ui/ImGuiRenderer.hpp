#pragma once

#ifdef AETHER_IMGUI

#	include <array>
#	include <cstdint>

#	include <imgui.h>

#	include "vulkan/Swapchain.hpp"

class ServiceContainer;
struct GLFWwindow;

namespace aether
{
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
		void Init(ServiceContainer& services, GLFWwindow* window);
		void Shutdown(ServiceContainer& services);

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
		void ReregisterPass(ServiceContainer& services);

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

		ServiceContainer* m_services = nullptr;
	};

} // namespace aether

#else // AETHER_IMGUI not defined - no-op stub, zero ImGui dependency

#	include <cstdint>

struct GLFWwindow;

class ServiceContainer;

namespace aether
{
	class ImGuiRenderer
	{
	public:
		void Init(ServiceContainer&, GLFWwindow*)
		{
		}

		void Shutdown(ServiceContainer&)
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

		void ReregisterPass(ServiceContainer&)
		{
		}
	};

} // namespace aether

#endif // AETHER_IMGUI
