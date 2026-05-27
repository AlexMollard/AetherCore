#include "ui/UISubsystem.hpp"

#include <string>

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether
{
	void UISubsystem::Init(ServiceContainer& services, std::string_view fontPath, std::string_view passPrefix, int glyphSize)
	{
		AE_PROFILE_ZONE();
		const std::string prefix(passPrefix);
		m_uiRenderer.Init(services, fontPath, prefix, glyphSize);
	}

	void UISubsystem::Shutdown(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		m_uiRenderer.Shutdown(services);
	}
} // namespace aether
