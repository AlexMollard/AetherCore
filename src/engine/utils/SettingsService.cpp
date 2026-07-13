#include "utils/SettingsService.hpp"

#include "AetherCore.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "platform/Window.hpp"
#include "rendering/Renderer.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether
{
	SettingsService::SettingsService(const EngineSettings& values, const EngineSettings& base, ServiceContainer& services)
	      : m_values(values), m_base(base), m_services(services)
	{
	}

	void SettingsService::ApplyLive(std::string_view key)
	{
		if (key == "graphics.fxaa")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetFxaaEnabled(m_values.graphics.fxaa);
			}
		}
		else if (key == "app.targetFps")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetTargetFps(m_values.app.targetFps);
			}
		}
		else if (key == "graphics.vsync")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetVsync(m_values.graphics.vsync); // no-op when unchanged
			}
		}
		else if (key == "graphics.imguiViewports")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetImguiViewportsEnabled(m_values.graphics.imguiViewports);
			}
		}
		else if (key == "graphics.uiScale")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetUiScale(m_values.graphics.uiScale);
			}
		}
		else if (key == "window.width" || key == "window.height")
		{
			// Window size is a scene-app setting (the game/editor resolution). A
			// UiShell tool front end (the Launcher) sizes its own window to its
			// content at the entry point and must not be stretched to the configured
			// resolution here - mirrors Application::BuildConfigFromSettings' UiShell
			// branch, which likewise keeps the entry point's size.
			if (const auto* engine = m_services.TryGet<AetherCore>(); engine != nullptr && engine->GetProfile() != RuntimeProfile::Full)
			{
				return;
			}
			if (auto* platform = m_services.TryGet<PlatformSubsystem>())
			{
				Window& window = platform->GetWindow();
				const FramebufferSize current = window.GetFramebufferSize();
				if (current.width != m_values.window.width || current.height != m_values.window.height)
				{
					window.SetSize(m_values.window.width, m_values.window.height);
				}
			}
		}
		// graphics.asyncCompute, app.startupScene, app.autoplay have no live effect;
		// they persist and take effect on next launch.
	}

	void SettingsService::ApplyField(std::string_view key)
	{
		ApplyLive(key);
		MarkDirty();
	}

	void SettingsService::ApplyAll()
	{
		AE_PROFILE_ZONE();
		ForEachSettingField(m_values, [this](std::string_view key, const auto&) { ApplyLive(key); });
	}

	void SettingsService::Save()
	{
		EngineSettingsIO::SaveUserOverrides(m_values, m_base);
		m_dirty = false;
	}
} // namespace aether
