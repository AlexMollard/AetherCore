#include "utils/SettingsService.hpp"
#include "ui/CursorService.hpp"

#include "AetherCore.hpp"
#include "platform/PlatformSubsystem.hpp"
#include "platform/Window.hpp"
#include "rendering/Renderer.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "passes/TonemapDefs.hpp"

namespace aether
{
	SettingsService::SettingsService(const EngineSettings& values, const EngineSettings& base, ServiceContainer& services)
	      : m_values(values), m_base(base), m_services(services)
	{
	}

	void SettingsService::ApplyLive(std::string_view key)
	{
		// The whole cursor block reconfigures together - the look is one thing, not six - and it has to
		// re-apply on every load, because a project's settings arrive well after the engine is built
		// (the editor opens a project seconds into the session).
		if (key.starts_with("cursor."))
		{
			if (auto* cursor = m_services.TryGet<ui::CursorService>())
			{
				cursor->Configure(m_values.cursor.custom,
				        {
				                .texture = m_values.cursor.texture,
				                .hotspot = {m_values.cursor.hotspotX, m_values.cursor.hotspotY},
				                .size = m_values.cursor.size,
				                .pixelArt = m_values.cursor.pixelArt,
				        });
			}
		}
		else if (key == "graphics.fxaa")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetFxaaEnabled(m_values.graphics.fxaa);
			}
		}
		else if (key == "graphics.shadowSplitLambda")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetShadowSplitLambda(m_values.graphics.shadowSplitLambda);
			}
		}
		else if (key == "graphics.volumetrics")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetVolumetricsEnabled(m_values.graphics.volumetrics);
			}
		}
		else if (key == "graphics.tonemap")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetTonemapMode(TonemapModeFromName(m_values.graphics.tonemap));
			}
		}
		else if (key == "graphics.reflections")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetReflectionsEnabled(m_values.graphics.reflections);
			}
		}
		else if (key == "graphics.reflectionMaxRoughness")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetReflectionMaxRoughness(m_values.graphics.reflectionMaxRoughness);
			}
		}
		else if (key == "graphics.reflectionIntensity")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetReflectionIntensity(m_values.graphics.reflectionIntensity);
			}
		}
		else if (key == "graphics.contactShadows")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetContactShadowsEnabled(m_values.graphics.contactShadows);
			}
		}
		else if (key == "graphics.gtao")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetGtaoEnabled(m_values.graphics.gtao);
			}
		}
		else if (key == "graphics.gtaoRadius")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetGtaoRadius(m_values.graphics.gtaoRadius);
			}
		}
		else if (key == "graphics.gtaoStrength")
		{
			if (auto* renderer = m_services.TryGet<Renderer>())
			{
				renderer->SetGtaoStrength(m_values.graphics.gtaoStrength);
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
				engine->SetVsync(m_values.graphics.vsync);
			}
		}
		else if (key == "graphics.lowLatencyPresent")
		{
			if (auto* engine = m_services.TryGet<AetherCore>())
			{
				engine->SetLowLatencyPresent(m_values.graphics.lowLatencyPresent);
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
			// content at the entry point and must not be stretched to the configured
			if (const auto* engine = m_services.TryGet<AetherCore>(); engine != nullptr && engine->GetProfile() != RuntimeProfile::Full)
			{
				return;
			}
			if (auto* platform = m_services.TryGet<PlatformSubsystem>())
			{
				Window& window = platform->GetWindow();
				// A borderless or fullscreen window is sized to the monitor by its mode. Resizing
				// it to the configured width and height shrinks it off the output, which both
				// loses the presentation path the mode exists for and forces a swapchain
				// recreate at the wrong size.
				if (window.GetMode() != Window::Mode::Windowed)
				{
					return;
				}
				// GetWindowSize, not GetFramebufferSize: SetSize takes screen coordinates, and on
				// a scaled display those are not pixels. Comparing pixels against a coordinate
				// setting made this fire on every apply and resize a window that was already right.
				const FramebufferSize current = window.GetWindowSize();
				if (current.width != m_values.window.width || current.height != m_values.window.height)
				{
					window.SetSize(m_values.window.width, m_values.window.height);
				}
			}
		}
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
