#include "Application.hpp"

#include <chrono>

#include "animation/AnimationSystem.hpp"
#include "assets/AssetManager.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "physics/PhysicsSystem.hpp"
#include "io/FileSystem.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/ShadowService.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "scene/BehaviorSystem.hpp"
#include "scene/LightSystem.hpp"
#include "systems/DayNightSystem.hpp"
#include "systems/ScriptComponentSystem.hpp"

namespace aether::app
{
	namespace
	{
		aether::AetherCore::Config BuildConfigFromSettings(const aether::AetherCore::Config& baseConfig, const aether::EngineSettings& settings)
		{
			aether::AetherCore::Config cfg = baseConfig;
			cfg.width = settings.window.width;
			cfg.height = settings.window.height;
			cfg.enableVsync = settings.graphics.vsync;
			return cfg;
		}

		void ConfigureTracyPlots()
		{
			AE_PROFILE_PLOT_CONFIG("Frame/ChannelSubmitNs", tracy::PlotFormatType::Number, false, true, 0x4EA3FF);
			AE_PROFILE_PLOT_CONFIG("Frame/GameThreadTotalNs", tracy::PlotFormatType::Number, false, true, 0xF7B955);
			AE_PROFILE_PLOT_CONFIG("Frame/RenderThreadExecNs", tracy::PlotFormatType::Number, false, true, 0x7BD88F);
			AE_PROFILE_PLOT_CONFIG("Animation/SampleJobs", tracy::PlotFormatType::Number, true, true, 0xE66A8A);
			AE_PROFILE_PLOT_CONFIG("Animation/SkinCopyJobs", tracy::PlotFormatType::Number, true, true, 0x9C7CFF);
			AE_PROFILE_PLOT_CONFIG("RenderQueue/TotalDraws", tracy::PlotFormatType::Number, true, true, 0x6ED3CF);
		}
	} // namespace

	void AppLayer::OnAttach(LayerContext& context)
	{
		(void) context;
	}

	void AppLayer::OnDetach(LayerContext& context)
	{
		(void) context;
	}

	void AppLayer::OnUpdate(LayerContext& context)
	{
		(void) context;
	}

	void AppLayer::OnImGui(LayerContext& context)
	{
		(void) context;
	}

	Application::Application(const aether::AetherCore::Config& engineConfig)
	      : Application(engineConfig, aether::EngineSettingsIO::LoadOrCreate(engineConfig.settingsFile))
	{
	}

	Application::Application(const aether::AetherCore::Config& engineConfig, const aether::EngineSettings& settings)
	      : m_settings(settings), m_engine(BuildConfigFromSettings(engineConfig, settings), settings)
	{
		AE_INFO(LogCategory::App, "Application created.");
	}

	Application::~Application()
	{
		if (!m_layersAttached)
		{
			AE_VERBOSE(LogCategory::App, "Application destroyed before layers were attached.");
			return;
		}

		LayerContext context{
		        .services = m_engine.GetServiceContainer(),
		        .deltaTimeSeconds = 0.0,
		        .elapsedTimeSeconds = 0.0,
		        .frameIndex = 0,
		};

		// Join the render thread AND wait the GPU idle BEFORE detaching layers,
		// which destroy GPU-referenced resources. StopRenderThread() does both.
		m_engine.StopRenderThread();

		// Reset the default executor so no more continuations are dispatched.
		aether::coro::set_default_executor(nullptr);

		// Unregister engine-level systems before detaching layers.
		context.Get<World>().UnregisterSystem("ScriptComponentSystem");
		context.Get<World>().UnregisterSystem("LightSystem");
		context.Get<World>().UnregisterSystem("DayNightSystem");
		context.Get<World>().UnregisterSystem("AnimationSystem");
		context.Get<World>().UnregisterSystem("PhysicsSystem");

		m_layers.DetachAll(context);
		AE_INFO(LogCategory::App, "Application shutdown complete.");
	}

	void Application::PushLayer(std::unique_ptr<AppLayer> layer)
	{
		m_layers.Push(std::move(layer));
		AE_VERBOSE(LogCategory::App, "Layer pushed to stack.");
	}

	int Application::Run()
	{
		AE_INFO(LogCategory::App, "Application run loop starting.");
		ConfigureTracyPlots();

		const auto shaderFilesResult = io::FileSystem::Glob("shaders://**/*.spv");
		if (!shaderFilesResult.has_value())
		{
			AE_WARN(LogCategory::FileSystem, "Shader glob failed: {}", shaderFilesResult.error().ToString());
		}
		else if (shaderFilesResult->empty())
		{
			AE_WARN(LogCategory::FileSystem, "No compiled shader files found via shaders://**/*.spv");
		}
		Logger::SetFrameNumber(0);

		// Set the coroutine default executor - all cross-thread continuation
		// resumptions (e.g. I/O thread -> game thread) go through this queue
		// and are drained at the top of each frame.
		// This is set up early so that any async operations during loading
		// dispatch correctly.
		aether::coro::set_default_executor(&m_coroExecutor);

		// Start the engine-owned render thread early (also registers the
		// RenderThread + IEngineRuntime services) so loading-screen frames can be
		// submitted while assets load incrementally and layers can resolve them.
		m_engine.StartRenderThread();

		LayerContext attachContext{
		        .services = m_engine.GetServiceContainer(),
		        .deltaTimeSeconds = 0.0,
		        .frameIndex = 0,
		};

		// Register engine-level systems before layers so any layer's OnRegister
		// can already find them (via ServiceContainer or World system lookup).
		{
			auto& services = attachContext.services;

			// Editor simulation state: the app boots frozen (Editing) unless the
			// autoplay setting flips it; panels toggle it via Play/Stop.
			m_playState.SetMode(m_settings.app.autoplay ? PlayState::Mode::Playing : PlayState::Mode::Editing);
			services.Register<PlayState>(m_playState);
			// Settings service: layers read app.startupScene; the scene UI's
			// set-as-startup writes it back through EngineSettingsIO::Save.
			services.Register<aether::EngineSettings>(m_settings);

			attachContext.Get<World>().RegisterSystem(std::make_unique<aether::AnimationSystem>());

			auto physicsSystem = std::make_unique<aether::PhysicsSystem>();
			auto physicsPtr = physicsSystem.get();
			attachContext.Get<World>().RegisterSystem(std::move(physicsSystem));
			services.Register<aether::PhysicsSystem>(*physicsPtr);

			auto dayNightSystem = std::make_unique<aether::app::DayNightSystem>();
			dayNightSystem->Init(*attachContext.TryGet<Renderer>());
			auto dayNightPtr = dayNightSystem.get();
			attachContext.Get<World>().RegisterSystem(std::move(dayNightSystem));
			services.Register<aether::app::DayNightSystem>(*dayNightPtr);

			// Entity lights -> renderer, every frame (see LightSystem).
			auto lightSystem = std::make_unique<aether::LightSystem>(*attachContext.TryGet<Renderer>());
			auto lightPtr = lightSystem.get();
			attachContext.Get<World>().RegisterSystem(std::move(lightSystem));
			services.Register<aether::LightSystem>(*lightPtr);

			// Data-driven scene behaviors (Bob/Spin/Orbit/MaterialPulse) - frozen
			// with the rest of the simulation while Editing.
			attachContext.Get<World>().RegisterSystem(std::make_unique<BehaviorSystem>(attachContext.Get<AssetManager>()));

			// Entity-attached scripts (ScriptComponent) - also play-gated. The
			// service registration is for the F5 path (handle invalidation).
			auto scriptSystem = std::make_unique<ScriptComponentSystem>(services);
			auto scriptPtr = scriptSystem.get();
			attachContext.Get<World>().RegisterSystem(std::move(scriptSystem));
			services.Register<ScriptComponentSystem>(*scriptPtr);
		}

		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		if (m_settings.app.targetFps > 0.0f)
		{
			AE_INFO(LogCategory::App, "Using settings TargetFPS={}.", m_settings.app.targetFps);
			m_engine.SetTargetFps(m_settings.app.targetFps);
		}
		else if (m_settings.graphics.vsync)
		{
			AE_INFO(LogCategory::App, "VSync is on and TargetFPS is 0 - using swapchain FIFO pacing.");
		}
		else
		{
			AE_INFO(LogCategory::App, "VSync is off and TargetFPS is 0 - frame pacer running uncapped.");
		}

		// Hand control to the engine-owned frame loop. This Application supplies
		// per-frame game logic, UI, and target invalidation through EngineClient
		// hooks; the engine owns the render thread, scheduling, and recreate.
		const int rc = m_engine.RunFrameLoop(*this);

		AE_INFO(LogCategory::App, "Application run loop exited.");
		return rc;
	}

	LayerContext Application::MakeLayerContext(double dtSeconds, std::uint64_t frameIndex)
	{
		return LayerContext{
		        .services = m_engine.GetServiceContainer(),
		        .deltaTimeSeconds = dtSeconds,
		        .elapsedTimeSeconds = 0.0, // layers do not read elapsed time
		        .frameIndex = frameIndex,
		};
	}

	void Application::OnFrameBegin()
	{
		// Resume any coroutines whose async I/O completed since the last frame.
		m_coroExecutor.drain();
	}

	double Application::GetTimeScale()
	{
		// Fast-forward: hold GraveAccent (` / ~) to multiply game speed. Polled
		// after input is updated (inside the engine's Tick), so it reads fresh.
		constexpr double kFastForwardScale = 10.0;
		const auto& input = m_engine.GetServiceContainer().Get<Input>();
		return input.IsKeyDown(Key::GraveAccent) ? kFastForwardScale : 1.0;
	}

	void Application::OnUpdate(double gameDt, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		LayerContext ctx = MakeLayerContext(gameDt, frameIndex);
		if (m_playState.IsPlaying())
		{
			ctx.Get<World>().UpdateSystems(static_cast<float>(gameDt));
		}
		else
		{
			// Edit mode: simulation systems (physics steps, animation time,
			// day/night) are frozen, but pending body descriptors still become
			// live bodies so loaded/created entities are pickable/teleportable.
			if (auto* physics = ctx.TryGet<aether::PhysicsSystem>())
			{
				physics->FlushPendingOnly(ctx.Get<World>());
			}
			// Day/night still APPLIES its current time to the renderer (dt = 0
			// advances nothing) so the panel's time-of-day scrub previews live
			// while paused.
			if (auto* dayNight = ctx.TryGet<aether::app::DayNightSystem>())
			{
				dayNight->Update(ctx.Get<World>(), 0.0f);
			}
			// Entity lights republish while paused too - light edits, gizmo
			// moves and freshly added lights preview live in the frozen scene.
			if (auto* lights = ctx.TryGet<aether::LightSystem>())
			{
				lights->Update(ctx.Get<World>(), 0.0f);
			}
		}
		m_layers.UpdateAll(ctx);
	}

	void Application::OnBuildUI(double gameDt, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		// gameDt (scaled) so the performance panel reflects fast-forward, matching
		// pre-refactor behaviour.
		LayerContext ctx = MakeLayerContext(gameDt, frameIndex);
		m_layers.ImGuiAll(ctx);
	}

	void Application::OnRenderTargetsInvalidated()
	{
		LayerContext ctx = MakeLayerContext(0.0, 0);
		m_layers.RenderTargetsInvalidatedAll(ctx);
	}

	aether::AetherCore& Application::GetEngine()
	{
		return m_engine;
	}

	const aether::AetherCore& Application::GetEngine() const
	{
		return m_engine;
	}
} // namespace aether::app
