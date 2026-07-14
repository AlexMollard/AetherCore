#include "editor/ControlServerLayer.hpp"

#include <cstdlib>
#include <string>

#include "editor/ControlServer.hpp"
#include "editor/ControlMethods.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	ControlServerLayer::ControlServerLayer() = default;
	ControlServerLayer::~ControlServerLayer() = default;

	void ControlServerLayer::OnAttach(app::LayerContext& context)
	{
		// Always construct + register the server so the editor's Control Server
		// panel can manage it (start/stop, port, live stats). It only opens a
		// socket once Start() is called - either from the env var below, or from
		// the panel.
		m_server = std::make_unique<ControlServer>(context.services, BuildControlMethods, "editor");
		context.services.Register<ControlServer>(*m_server);

		// Auto-start when AETHER_CONTROL_PORT names a port (scripted / headless
		// use, e.g. the MCP or aether-ctl driving the editor unattended).
		const char* portEnv = std::getenv("AETHER_CONTROL_PORT");
		if (portEnv == nullptr || *portEnv == '\0')
		{
			return;
		}

		int port = 0;
		try
		{
			port = std::stoi(portEnv);
		}
		catch (const std::exception&)
		{
			AE_WARN(LogCategory::App, "ControlServer: AETHER_CONTROL_PORT='{}' is not a valid port; not auto-starting.", portEnv);
			return;
		}
		if (port <= 0 || port > 65535)
		{
			AE_WARN(LogCategory::App, "ControlServer: AETHER_CONTROL_PORT={} out of range; not auto-starting.", port);
			return;
		}
		m_server->Start(port);
	}

	void ControlServerLayer::OnUpdate(app::LayerContext& context)
	{
		if (m_server == nullptr || !m_server->IsRunning())
		{
			return;
		}
		const double fps = context.deltaTimeSeconds > 0.0 ? 1.0 / context.deltaTimeSeconds : 0.0;
		m_server->SetFrameInfo(context.frameIndex, fps);
		m_server->DrainCommands();
	}

	void ControlServerLayer::OnDetach(app::LayerContext& context)
	{
		if (m_server != nullptr)
		{
			context.services.Unregister<ControlServer>();
			m_server->Stop();
			m_server.reset();
		}
	}
} // namespace aether::editor
