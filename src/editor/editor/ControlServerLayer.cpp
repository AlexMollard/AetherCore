#include "editor/ControlServerLayer.hpp"

#include <cstdlib>
#include <string>

#include "editor/ControlServer.hpp"
#include "editor/ControlMethods.hpp"
#include "io/PlatformPaths.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	ControlServerLayer::ControlServerLayer() = default;
	ControlServerLayer::~ControlServerLayer() = default;

	void ControlServerLayer::OnAttach(app::LayerContext& context)
	{
		m_server = std::make_unique<ControlServer>(context.services, BuildControlMethods, "editor");
		context.services.Register<ControlServer>(*m_server);

		const std::string portEnv = io::PlatformPaths::ReadEnvironmentVariable("AETHER_CONTROL_PORT");
		if (portEnv.empty())
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
