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
	//   Game thread  : BeginFrame() -> [layer OnGui calls] -> SnapshotFrame()
	//                  -> RenderPlatformWindows() (secondary OS windows)
	//   Render thread : registered RenderGraph pass calls RenderSlot()
	//
	// SnapshotFrame deep-copies ImGui's draw data into a per-slot owned buffer.
	// This eliminates the data dependency between threads: the game thread can
	// immediately call ImGui::NewFrame() (which invalidates ImGui's internal
	// draw data) without waiting for the render thread to finish.
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
		// Finalises the ImGui frame and deep-copies all draw data into
		// the current write slot so the render thread can consume it
		// independently of ImGui's internal data lifetime.
		void SnapshotFrame();

		// Game thread - after SnapshotFrame, before SubmitFrame.
		// Renders secondary OS windows (ViewportsEnable) using imgui_impl_vulkan.
		void RenderPlatformWindows();

		void ReregisterPass(ServiceContainer& services);

	private:
		// Owned deep-copy of a single ImGui frame's draw data.
		// All memory is allocated via IM_ALLOC / ImGui::MemAlloc so that
		// ImVector destructors (~ImDrawList) free it correctly via IM_FREE.
		struct ImGuiSlot
		{
			ImDrawData drawData;
			bool valid = false;

			void Free();
		};

		void RenderSlot(PassContext& ctx, uint32_t slot);

		std::array<ImGuiSlot, Swapchain::kMaxFramesInFlight> m_slots;
		uint32_t m_writeSlot = 0;

		ServiceContainer* m_services = nullptr;
	};

} // namespace aether

#else // AETHER_IMGUI not defined - no-op stub

#	include <cstdint>

struct GLFWwindow;
class ServiceContainer;

namespace aether
{
	class ImGuiRenderer
	{
	public:
		void Init(ServiceContainer&, GLFWwindow*) {}
		void Shutdown(ServiceContainer&) {}
		void SetWriteSlot(uint32_t) {}
		void BeginFrame() {}
		void SnapshotFrame() {}
		void RenderPlatformWindows() {}
		void ReregisterPass(ServiceContainer&) {}
	};

} // namespace aether

#endif // AETHER_IMGUI
