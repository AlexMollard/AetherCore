#pragma once

#include <memory>

#include "layers/AppLayer.hpp"

namespace aether::editor
{
	class ControlServer;

	// Owns the editor ControlServer across the app lifetime: starts it on attach
	class ControlServerLayer : public app::AppLayer
	{
	public:
		ControlServerLayer();
		~ControlServerLayer() override;

		void OnAttach(app::LayerContext& context) override;
		void OnUpdate(app::LayerContext& context) override;
		void OnDetach(app::LayerContext& context) override;

	private:
		std::unique_ptr<ControlServer> m_server;
	};
} // namespace aether::editor
