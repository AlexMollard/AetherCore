#pragma once

#include <memory>

#include "scene/AetherCore.hpp"
#include "AppLayer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/FramePacer.hpp"
#include "LayerStack.hpp"
#include "rendering/RenderThread.hpp"
#include "ui/ImGuiRenderer.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiSystem.hpp"
#include "ui/UiWorld.hpp"

namespace aether::app
{
	class Application
	{
	public:
		explicit Application(const aether::AetherCore::Config& engineConfig = {});
		~Application();

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		void PushLayer(std::unique_ptr<AppLayer> layer);
		int Run();

		// Set the target frame rate for the game thread.  Pass 0 (the default) to run uncapped.
		void SetTargetFps(float fps)
		{
			m_framePacer.SetTargetFps(fps);
		}

		[[nodiscard]] float GetTargetFps() const
		{
			return m_framePacer.GetTargetFps();
		}

		[[nodiscard]] aether::AetherCore& GetEngine();
		[[nodiscard]] const aether::AetherCore& GetEngine() const;

	private:
		Application(const aether::AetherCore::Config& engineConfig, const aether::EngineSettings& settings);

		aether::EngineSettings m_settings;
		aether::AetherCore m_engine;
		aether::RenderThread m_renderThread;
		aether::FramePacer m_framePacer;
		aether::UIRenderer m_uiRenderer;
		aether::ImGuiRenderer m_imguiRenderer;
		aether::ui::UiWorld m_uiWorld;
		aether::ui::UiContext m_uiContext;
		aether::ui::UiSystem m_uiSystem;
		LayerStack m_layers;
		bool m_layersAttached = false;
		std::uint64_t m_frameIndex = 0;
	};
} // namespace aether::app
