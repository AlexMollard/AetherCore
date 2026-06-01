#pragma once

#include <memory>

#include "AetherCore.hpp"
#include "layers/AppLayer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/FramePacer.hpp"
#include "utils/LoadingManager.hpp"
#include "utils/coro/Executor.hpp"
#include "layers/LayerStack.hpp"
#include "rendering/RenderThread.hpp"

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
	aether::coro::queued_executor m_coroExecutor;
		aether::LoadingManager m_loadingManager;
		LayerStack m_layers;
		bool m_layersAttached = false;
		std::uint64_t m_frameIndex = 0;
		double m_elapsedTimeSeconds = 0.0;
	};
} // namespace aether::app
