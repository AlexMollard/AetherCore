#pragma once

#include <memory>
#include <type_traits>

#include "AetherCore.hpp"
#include "PlayState.hpp"
#include "layers/AppLayer.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/FramePacer.hpp"
#include "utils/coro/Executor.hpp"
#include "layers/LayerStack.hpp"
#include "rendering/RenderThread.hpp"

namespace aether::app
{
	// The application is now a thin EngineClient: it owns layers and game/editor
	// policy, and plugs into the engine-owned frame loop via hooks. The engine
	// (AetherCore) owns the render thread, frame scheduling, and resource
	// lifecycle.
	class Application : public aether::EngineClient
	{
	public:
		explicit Application(const aether::AetherCore::Config& engineConfig = {});
		~Application() override;

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		// Push a layer that does not need to be looked up by service-locator.
		void PushLayer(std::unique_ptr<AppLayer> layer);

		// Construct a layer in place from the given args and push it.
		// Saves the caller from wrapping in std::make_unique.
		template<typename T, typename... Args>
		    requires std::is_base_of_v<AppLayer, T> && std::constructible_from<T, Args&&...>
		T& PushLayer(Args&&... args)
		{
			return PushOwnedLayer(std::make_unique<T>(std::forward<Args>(args)...));
		}

		// Push a layer and register it in the ServiceContainer under its concrete type.
		// Use when a layer needs to be reachable via TryGet<T>() (e.g. by a script
		// binding, a script's SceneContext, or another layer).
		template<typename T>
		    requires std::is_base_of_v<AppLayer, T>
		T& PushOwnedLayer(std::unique_ptr<T> layer)
		{
			T& ref = *layer;
			m_engine.GetServiceContainer().Register<T>(ref);
			m_layers.Push(std::move(layer));
			return ref;
		}

		// Register a pre-existing service instance (e.g. a stack-allocated subsystem).
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

		// --- EngineClient hooks (called by AetherCore::RunFrameLoop) -----------
		void OnFrameBegin() override;
		double GetTimeScale() override;
		void OnUpdate(double gameDt, std::uint64_t frameIndex) override;
		void OnBuildUI(double gameDt, std::uint64_t frameIndex) override;
		void OnRenderTargetsInvalidated() override;

		// Shortcut to the engine's ServiceContainer.
		[[nodiscard]] aether::ServiceContainer& Services()
		{
			return m_engine.GetServiceContainer();
		}

	private:
		Application(const aether::AetherCore::Config& engineConfig, const aether::EngineSettings& settings);

		// Builds a LayerContext for the given per-frame timing. Layers do not read
		// elapsedTimeSeconds, so it is left at 0.
		[[nodiscard]] LayerContext MakeLayerContext(double dtSeconds, std::uint64_t frameIndex);

		aether::EngineSettings m_settings;
		aether::AetherCore m_engine;
		aether::coro::queued_executor m_coroExecutor;
		LayerStack m_layers;
		PlayState m_playState;
		bool m_layersAttached = false;
	};
} // namespace aether::app
