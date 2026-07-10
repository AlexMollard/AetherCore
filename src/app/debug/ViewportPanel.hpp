#pragma once

#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>

#include "debug/DebugPanel.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::app
{
	class ViewportPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Viewport";
		}

		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnImGui(LayerContext& context) override;
		void OnRenderTargetsInvalidated(LayerContext& context) override;

	private:
		void ReleaseSceneViewportTexture(LayerContext& context);
		// Click-to-select. Must be called while the viewport InvisibleButton is
		// still ImGui's last item (hover/click state reads from it).
		void HandleViewportPicking(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// ImGuizmo manipulator on the primary selection, drawn over the image.
		// Returns true when a gizmo was actually submitted this frame (valid
		// selection + camera), so the caller can trust ImGuizmo's IsOver/IsUsing
		// state - those are stale leftovers on frames where Manipulate never ran.
		bool DrawTransformGizmo(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// Play/Stop toolbar buttons (snapshot on Play, restore on Stop).
		void DrawPlayControls(LayerContext& context);
		// Wireframe frustums for every entity camera, drawn over the scene image
		// (main camera highlighted, selected brightened). Purely an overlay.
		void DrawCameraGizmos(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// "Look through selected camera" overlay button + exit control. Toggling
		// sets m_lookThroughEntityId, which OnUpdate uses to lock the editor camera.
		void DrawCameraPreviewControls(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize);

		std::uint64_t m_sceneViewportTextureId = 0;
		gpu::ImageView m_sceneViewportImageView = nullptr;

		int m_viewportDisplayMode = 0; // Fit, fill, actual, integer
		int m_viewportAspectMode = 0;  // Render, free, 16:9, 16:10, 4:3, 1:1
		bool m_viewportShowStats = true;
		bool m_viewportShowMouse = true;

		int m_gizmoOp = 0;         // 0 translate, 1 rotate, 2 scale
		bool m_gizmoLocal = false; // world-space handles by default

		// Editor free-fly camera: takes over as main while Editing (seeded from
		// the game camera's view), hands back on Play. Stored as raw ids to keep
		// this header camera-include-free.
		std::uint32_t m_editorCamId = 0;
		std::uint32_t m_gameCamId = 0;
		bool m_editorCamActive = false;

		// While Editing, locks the editor camera to this camera entity's pose+fov
		// each frame (live "look through" preview). 0 = not previewing.
		std::uint32_t m_lookThroughEntityId = 0;
	};
} // namespace aether::app
