#pragma once

#include <memory>
#include <type_traits>

#include "AetherCore.hpp"
#include "PlayState.hpp"
#include "layers/AppLayer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/SettingsService.hpp"
#include "utils/FramePacer.hpp"
#include "utils/coro/Executor.hpp"
#include "layers/LayerStack.hpp"
#include "rendering/RenderThread.hpp"

namespace aether::app
{
	// (AetherCore) owns the render thread, frame scheduling, and resource
	class Application : public aether::EngineClient
	{
	public:
		explicit Application(const aether::AetherCore::Config& engineConfig = {});
		~Application() override;

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		void PushLayer(std::unique_ptr<AppLayer> layer);

		template<typename T, typename... Args>
		    requires std::is_base_of_v<AppLayer, T> && std::constructible_from<T, Args&&...>
		T& PushLayer(Args&&... args)
		{
			return PushOwnedLayer(std::make_unique<T>(std::forward<Args>(args)...));
		}

		template<typename T>
		    requires std::is_base_of_v<AppLayer, T>
		T& PushOwnedLayer(std::unique_ptr<T> layer)
		{
			T& ref = *layer;
			m_engine.GetServiceContainer().Register<T>(ref);
			m_layers.Push(std::move(layer));
			return ref;
		}

		template<typename T>
		void AddService(T& service)
		{
			m_engine.GetServiceContainer().Register<T>(service);
		}

		int Run();

		void SetTargetFps(float fps)
		{
			m_engine.SetTargetFps(fps);
		}

		[[nodiscard]] float GetTargetFps() const
		{
			return m_engine.GetTargetFps();
		}

		[[nodiscard]] aether::AetherCore& GetEngine();
		[[nodiscard]] const aether::AetherCore& GetEngine() const;

		void OnFrameBegin() override;
		double GetTimeScale() override;
		void OnUpdate(double gameDt, std::uint64_t frameIndex) override;
		void OnBuildUI(double gameDt, std::uint64_t frameIndex) override;
		void OnRenderTargetsInvalidated() override;

		[[nodiscard]] aether::ServiceContainer& Services()
		{
			return m_engine.GetServiceContainer();
		}

	private:
		Application(const aether::AetherCore::Config& engineConfig, const aether::LoadedEngineSettings& loaded);

		[[nodiscard]] LayerContext MakeLayerContext(double dtSeconds, std::uint64_t frameIndex);

		aether::AetherCore m_engine;
		aether::SettingsService m_settingsService;
		aether::coro::queued_executor m_coroExecutor;
		LayerStack m_layers;
		PlayState m_playState;
		bool m_layersAttached = false;
	};
} // namespace aether::app
