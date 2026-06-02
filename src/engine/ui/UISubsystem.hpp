#pragma once

#include <string_view>

#include "ui/UIRenderer.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiSystem.hpp"

namespace aether
{
	class ServiceContainer;
}

namespace aether
{
	// Owns the engine UI systems (UIRenderer, UiContext, UiSystem).
	// Initialized after the rendering subsystem so that Vulkan resources
	// are available for UI pipeline creation.
	class UISubsystem
	{
	public:
		void Init(ServiceContainer& services, std::string_view fontPath, std::string_view passPrefix = "UIPass", int glyphSize = 48);
		void Shutdown(ServiceContainer& services);

		[[nodiscard]] UIRenderer& GetUiRenderer()
		{
			return m_uiRenderer;
		}

		[[nodiscard]] ui::UiContext& GetUiContext()
		{
			return m_uiContext;
		}

		[[nodiscard]] ui::UiSystem& GetUiSystem()
		{
			return m_uiSystem;
		}

	private:
		UIRenderer m_uiRenderer;
		ui::UiContext m_uiContext;
		ui::UiSystem m_uiSystem;
	};
} // namespace aether
