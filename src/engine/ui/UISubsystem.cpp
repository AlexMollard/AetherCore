#include "ui/UISubsystem.hpp"

#include <string>

#include "ServiceContainer.hpp"

namespace aether
{
	void UISubsystem::Init(ServiceContainer& services, std::string_view fontPath, std::string_view passPrefix, int glyphSize)
	{
		const std::string prefix(passPrefix);
		m_uiRenderer.Init(services, fontPath, prefix, glyphSize);
	}

	void UISubsystem::Shutdown(ServiceContainer& services)
	{
		m_uiRenderer.Shutdown(services);
	}
} // namespace aether
