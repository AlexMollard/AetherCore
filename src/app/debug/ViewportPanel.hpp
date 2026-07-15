#pragma once

#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>

#include "debug/DebugPanel.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::editor
{
	class ViewportPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Viewport";
		}

		void OnAttach(app::LayerContext& context) override;
		void OnDetach(app::LayerContext& context) override;
		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;
		void OnRenderTargetsInvalidated(app::LayerContext& context) override;

	private:
		void ReleaseSceneViewportTexture(app::LayerContext& context);
		// Click-to-select. Must be called while the viewport InvisibleButton is current.
		void HandleViewportPicking(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// Returns whether a gizmo was submitted so callers do not use stale ImGuizmo state.
		bool DrawTransformGizmo(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		void Draw2DGrid(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		void Handle2DNavigation(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		void DrawPlayControls(app::LayerContext& context);
		void DrawCameraGizmos(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		void DrawSpriteOutlines(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// sets m_lookThroughEntityId, which OnUpdate uses to lock the editor camera.
		void DrawCameraPreviewControls(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize);
		void RestoreEditorCameraAfterLookThrough(app::LayerContext& context);

		std::uint64_t m_sceneViewportTextureId = 0;
		gpu::ImageView m_sceneViewportImageView = nullptr;

		std::uint64_t m_cameraPreviewTextureId = 0;
		gpu::ImageView m_cameraPreviewImageView = nullptr;

		int m_viewportDisplayMode = 0;
		int m_viewportAspectMode = 0;
		bool m_viewportShowStats = true;
		bool m_viewportShowMouse = true;
		bool m_viewportShow2DGrid = true;

		int m_gizmoOp = 0;
		bool m_gizmoLocal = false;

		std::uint32_t m_editorCamId = 0;
		std::uint32_t m_gameCamId = 0;
		bool m_editorCamActive = false;
		bool m_editor2DMode = false;

		std::uint32_t m_lookThroughEntityId = 0;
		glm::vec3 m_saved2DEditorPosition{0.0f, 0.0f, 10.0f};
		float m_saved2DEditorHeight = 10.0f;
		bool m_hasSaved2DEditorCamera = false;
	};
} // namespace aether::editor
