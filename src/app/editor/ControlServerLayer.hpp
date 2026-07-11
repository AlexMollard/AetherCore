#pragma once

#include <memory>

#include "layers/AppLayer.hpp"

namespace aether::app::editor
{
	class ControlServer;

	// Owns the editor ControlServer across the app lifetime: starts it on attach
	// (only when the AETHER_CONTROL_PORT environment variable names a port),
	// drains its request queue every frame on the main thread, and stops it on
	// detach. Editor-only - lives under src/app/editor, which GameRuntime's source
	// list excludes, so a shipped game never carries the control endpoint.
	class ControlServerLayer : public AppLayer
	{
	public:
		ControlServerLayer();
		~ControlServerLayer() override;

		void OnAttach(LayerContext& context) override;
		void OnUpdate(LayerContext& context) override;
		void OnDetach(LayerContext& context) override;

	private:
		std::unique_ptr<ControlServer> m_server;
	};
} // namespace aether::app::editor
