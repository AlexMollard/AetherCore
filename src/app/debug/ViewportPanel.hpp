#pragma once

#include <cstdint>
#include <vector>
#include <string_view>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "debug/DebugPanel.hpp"
#include "debug/TilePaintingState.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether::app
{
	class PlayState;
}

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
		// Record a just-released gizmo drag as one TransformCommand. No-op when nothing moved.
		void FinishGizmoDrag(app::LayerContext& context);
		void Draw2DGrid(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		void Handle2DNavigation(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		void DrawPlayControls(app::LayerContext& context);
		// Total content width the play controls need this frame (Play; or Stop +
		// Pause/Resume + Step while playing). Keeps the centered toolbar pill sized
		// to match what DrawPlayControls renders.
		[[nodiscard]] static float PlayControlsContentWidth(const app::PlayState* playState);
		// Play-mode overlay in the viewport corner: state (Playing/Paused), elapsed
		// sim time, frame count, and FPS. Drawn only while a session is live.
		void DrawPlayHud(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize);
		void DrawCameraGizmos(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// Interactive Collider 2D editing for the selected entity in 2D edit mode:
		// box/circle/capsule size handles, offset handle, and polygon point
		// drag/insert (ctrl+click edge)/remove (right-click point).
		void DrawCollider2DHandles(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// Tile painting for the selected Tile Map entity (tool state lives in the
		// TilePaintingState service; strokes record chunk-local diffs for undo).
		void HandleTilePainting(app::LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
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
		// Maximize on Play: fullscreen the viewport while a session runs. m_wasMaximized
		// tracks the transition so focus is only stolen on the frame it turns on.
		bool m_maximizeOnPlay = false;
		bool m_wasMaximized = false;
		// The play-state overlay (state / elapsed / FPS) sits over the top-left of the game view,
		// which is exactly where a lot of games put their own HUD. Toggleable so it can be moved out
		// of the way while looking at the game rather than at the session. Off by default: the game's
		// own HUD lives in that corner, and the same numbers are in the Performance panel.
		bool m_viewportShowPlayHud = false;
		bool m_viewportShowMouse = true;
		bool m_viewportShow2DGrid = true;

		int m_gizmoOp = 0;
		bool m_gizmoLocal = false;

		// Gizmo drag -> exactly one TransformCommand per drag. While the gizmo is
		// idle the affected entities' world matrices are re-snapshotted every frame,
		// so the frame a drag starts already holds its pre-drag state; the command is
		// recorded when the drag releases (a multi-frame drag stays one undo step).
		struct GizmoDragEntry
		{
			std::uint32_t id = 0;
			glm::mat4 before{1.0f};
		};

		bool m_gizmoDragging = false;
		std::vector<GizmoDragEntry> m_gizmoDragBefore;

		std::uint32_t m_editorCamId = 0;
		std::uint32_t m_gameCamId = 0;
		bool m_editorCamActive = false;
		bool m_editor2DMode = false;

		// Collider 2D handle interaction (edit mode). Capture suppresses viewport
		// picking while a handle is hovered or dragged.
		int m_collider2DActiveHandle = -1;
		bool m_collider2DMouseCapture = false;
		// Set once a gesture has snapshotted the collider; m_collider2DBefore then holds
		// the pre-edit fields so the whole gesture records as one SetComponentCommand.
		bool m_collider2DUndoPushed = false;
		nlohmann::json m_collider2DBefore;
		bool m_collider2DBeforeReflected = false;

		// Tile painting gesture state (tool selection lives in TilePaintingState).
		bool m_tilePaintCapture = false;
		bool m_tileStrokeActive = false;
		bool m_tileStrokeErasing = false; // this drag was started with RMB (erase)
		bool m_tileRectDragging = false;
		bool m_tileRectErasing = false; // this rect drag was started with RMB (erase)
		glm::ivec2 m_tileRectAnchor{0};
		std::vector<glm::ivec2> m_tileStrokeCells;
		std::vector<editor::TilePaintEdit> m_tileStrokeEdits;

		std::uint32_t m_lookThroughEntityId = 0;
		glm::vec3 m_saved2DEditorPosition{0.0f, 0.0f, 10.0f};
		float m_saved2DEditorHeight = 10.0f;
		bool m_hasSaved2DEditorCamera = false;
	};
} // namespace aether::editor
