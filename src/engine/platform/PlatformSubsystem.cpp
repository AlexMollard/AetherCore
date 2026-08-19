#include "platform/PlatformSubsystem.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void PlatformSubsystem::Init(const Config& config)
	{
		AE_PROFILE_ZONE();
		m_window.emplace(config.appName, config.width, config.height, config.mode, config.startHidden);
		m_input.Init(m_window->GetHandle());
	}

	void PlatformSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_window.reset();
	}
} // namespace aether
