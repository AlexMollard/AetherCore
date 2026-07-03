#pragma once

#include <cstdint>
#include <string_view>

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

		std::uint64_t m_sceneViewportTextureId = 0;
		gpu::ImageView m_sceneViewportImageView = nullptr;

		int m_viewportDisplayMode = 0; // Fit, fill, actual, integer
		int m_viewportAspectMode = 0;  // Render, free, 16:9, 16:10, 4:3, 1:1
		bool m_viewportShowStats = true;
		bool m_viewportShowMouse = true;
	};
} // namespace aether::app
