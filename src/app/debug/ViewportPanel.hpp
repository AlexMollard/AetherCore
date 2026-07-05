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
		void OnImGui(LayerContext& context) override;
		void OnRenderTargetsInvalidated(LayerContext& context) override;

	private:
		void ReleaseSceneViewportTexture(LayerContext& context);
		// Click-to-select. Must be called while the viewport InvisibleButton is
		// still ImGui's last item (hover/click state reads from it).
		void HandleViewportPicking(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);
		// ImGuizmo manipulator on the primary selection, drawn over the image.
		void DrawTransformGizmo(LayerContext& context, glm::vec2 imageMin, glm::vec2 imageSize, float renderAspect);

		std::uint64_t m_sceneViewportTextureId = 0;
		gpu::ImageView m_sceneViewportImageView = nullptr;

		int m_viewportDisplayMode = 0; // Fit, fill, actual, integer
		int m_viewportAspectMode = 0;  // Render, free, 16:9, 16:10, 4:3, 1:1
		bool m_viewportShowStats = true;
		bool m_viewportShowMouse = true;

		int m_gizmoOp = 0;         // 0 translate, 1 rotate, 2 scale
		bool m_gizmoLocal = false; // world-space handles by default
	};
} // namespace aether::app
