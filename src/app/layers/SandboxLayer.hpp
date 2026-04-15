#pragma once

#include <string_view>

#include "AppLayer.hpp"
#include "CameraManager.hpp"

namespace aether::app
{
	class SandboxGameSystem;

	// Application layer for the sandbox scene.
	// Coordinates with SandboxGameSystem for all actual game logic.
	// Also handles debug UI rendering.
	class SandboxLayer final : public AppLayer
	{
	public:
		void OnAttach(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnGui(LayerContext& context) override;

	private:
		const char* GetActiveCameraName(aether::CameraHandle activeCamera) const;
		void DrawDebugLine(aether::UIRenderer& ui, std::string_view text, float y) const;

		SandboxGameSystem* m_gameSystem = nullptr;
	};
}