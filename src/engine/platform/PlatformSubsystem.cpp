#include "platform/PlatformSubsystem.hpp"

namespace aether
{
	void PlatformSubsystem::Init(const Config& config)
	{
		m_window.emplace(config.appName, config.width, config.height);
		m_input.Init(m_window->GetHandle());
	}

	void PlatformSubsystem::Shutdown()
	{
		m_window.reset();
	}
} // namespace aether
