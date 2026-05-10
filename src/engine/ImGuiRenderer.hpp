#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <imgui.h>

#include "Swapchain.hpp"

struct GLFWwindow;

namespace aether
{
	class AetherCore;
	struct PassContext;

	// Integrates Dear ImGui (docking branch) with AetherCore's render graph.
	//
	// Threading model:
	//   Game thread : BeginFrame() → [layer OnGui calls] → SnapshotFrame()
	//                 → RenderPlatformWindows() (secondary OS windows)
	//   Render thread : registered RenderGraph pass calls RenderSlot()
	//
	// SnapshotFrame deep-copies per-draw-list vertex/index/command data into
	// per-slot buffers so the render thread can consume them safely without
	// accessing live ImGui state.
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
		// Finalises the ImGui frame and deep-copies draw data for the render thread.
		void SnapshotFrame();

		// Game thread - after SnapshotFrame, before SubmitFrame.
		// Renders secondary OS windows (ViewportsEnable) using imgui_impl_vulkan.
		void RenderPlatformWindows();

		// Called from the swapchain-recreated callback to re-register the render
		// graph pass (the graph is cleared on every swapchain resize).
		void ReregisterPass(AetherCore& engine);

	private:
		// Deep copy of one ImDrawList, safe to read from a different thread.
		struct ListCopy
		{
			ImVector<ImDrawVert> vtx;
			ImVector<ImDrawIdx> idx;
			ImVector<ImDrawCmd> cmds;
			ImDrawListFlags flags = 0;
		};

		struct FrameSlot
		{
			std::vector<ListCopy> lists;
			ImVec2 displayPos{};
			ImVec2 displaySize{};
			ImVec2 fbScale{ 1.f, 1.f };
			// Pointer to the shared texture list (ImGui::GetPlatformIO().Textures).
			// Needed by imgui_impl_vulkan to upload pending textures during RenderDrawData.
			ImVector<ImTextureData*>* textures = nullptr;
			bool hasData = false;

			// Temporary ImDrawList objects built during rendering (owned per slot,
			// filled from the ListCopy data above so imgui_impl_vulkan can iterate).
			std::vector<std::unique_ptr<ImDrawList>> tempLists;
		};

		void RenderSlot(PassContext& ctx, uint32_t slot);

		std::array<FrameSlot, Swapchain::kMaxFramesInFlight> m_slots;
		uint32_t m_writeSlot = 0;

		// Borrowed pointers into the ImGui context - valid until DestroyContext().
		ImDrawListSharedData* m_sharedData = nullptr;
		ImGuiViewport* m_mainViewport = nullptr;

		AetherCore* m_engine = nullptr;
	};

} // namespace aether
