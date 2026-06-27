#include "Application.hpp"

#include <chrono>

#include "animation/AnimationSystem.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "physics/PhysicsSystem.hpp"
#include "io/FileSystem.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/ShadowService.hpp"
#include "scene/World.hpp"
#include "ui/UiContext.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiSystem.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "systems/DayNightSystem.hpp"

namespace aether::app
{
	namespace
	{
		constexpr std::string_view kUiFontPath = "assets://fonts/Roboto-Regular.ttf";

		aether::AetherCore::Config BuildConfigFromSettings(const aether::AetherCore::Config& baseConfig, const aether::EngineSettings& settings)
		{
			aether::AetherCore::Config cfg = baseConfig;
			cfg.width = settings.window.width;
			cfg.height = settings.window.height;
			cfg.enableVsync = settings.graphics.vsync;
			if (cfg.uiFontPath == nullptr || cfg.uiFontPath[0] == '\0')
			{
				cfg.uiFontPath = kUiFontPath.data();
			}
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

	void AppLayer::OnGui(LayerContext& context)
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
		        .frameIndex = m_frameIndex,
		};

		m_renderThread.Stop();
		m_engine.WaitIdle();

		// Reset the default executor so no more continuations are dispatched.
		aether::coro::set_default_executor(nullptr);

		// Unregister engine-level systems before detaching layers.
		context.Get<World>().UnregisterSystem("DayNightSystem");
		context.Get<World>().UnregisterSystem("AnimationSystem");
		context.Get<World>().UnregisterSystem("PhysicsSystem");

		m_layers.DetachAll(context);
		// Engine UIRenderer shutdown is handled by AetherCore.
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

		// Start the dedicated render thread early so that loading-screen
		// frames can be submitted while assets load incrementally.
		m_renderThread.Start(m_engine);
		m_engine.GetServiceContainer().Register<aether::RenderThread>(m_renderThread);

		LayerContext attachContext{
		        .services = m_engine.GetServiceContainer(),
		        .deltaTimeSeconds = 0.0,
		        .frameIndex = 0,
		};

		// Register engine-level systems before layers so any layer's OnRegister
		// can already find them (via ServiceContainer or World system lookup).
		{
			auto& services = attachContext.services;
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
		}

		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		if (m_settings.app.targetFps > 0.0f)
		{
			AE_INFO(LogCategory::App, "Using settings TargetFPS={}.", m_settings.app.targetFps);
			m_framePacer.SetTargetFps(m_settings.app.targetFps);
		}
		else if (m_settings.graphics.vsync)
		{
			AE_INFO(LogCategory::App, "VSync is on and TargetFPS is 0 - using swapchain FIFO pacing.");
		}
		else
		{
			AE_INFO(LogCategory::App, "VSync is off and TargetFPS is 0 - frame pacer running uncapped.");
		}

		auto previousFrameTime = std::chrono::steady_clock::now();

		while (!m_engine.ShouldClose())
		{
			AE_PROFILE_ZONE();

			const auto frameStartTime = std::chrono::steady_clock::now();

			m_framePacer.Wait();

			// Resume any coroutines whose async I/O completed on the background
			// thread since the last frame.
			m_coroExecutor.drain();

			Logger::SetFrameNumber(m_frameIndex);

			// Measure dt before PumpEvents so drag stalls and window-event jitter
			// don't inflate simulation timing or cause camera/object jumps.
			constexpr double kMaxDeltaTime = 1.0 / 30.0;
			const auto currentFrameTime = std::chrono::steady_clock::now();
			const auto deltaTime = std::min(std::chrono::duration<double>(currentFrameTime - previousFrameTime).count(), kMaxDeltaTime);
			previousFrameTime = currentFrameTime;

			m_engine.PumpEvents();

			// Update engine-level per-frame systems (input + camera).
			// Camera uses unscaled dt so it stays controllable during fast-forward.
			m_engine.Tick(static_cast<float>(deltaTime));

			// Fast-forward: hold GraveAccent (` / ~) to multiply game speed.
			constexpr double kFastForwardScale = 10.0;
			const auto& input = m_engine.GetServiceContainer().Get<Input>();
			const double timeScale = input.IsKeyDown(Key::GraveAccent) ? kFastForwardScale : 1.0;
			const double scaledDt = deltaTime * timeScale;
			m_elapsedTimeSeconds += scaledDt;

			LayerContext frameContext{
			        .services = m_engine.GetServiceContainer(),
			        .deltaTimeSeconds = scaledDt,
			        .elapsedTimeSeconds = m_elapsedTimeSeconds,
			        .frameIndex = m_frameIndex,
			};

			// Update ECS systems (game logic) with scaled dt.
			{
				AE_PROFILE_ZONE();
				frameContext.Get<World>().UpdateSystems(static_cast<float>(scaledDt));
			}

			// Compute the CPU double-buffer write slot for this frame.
			const auto drawSlot = static_cast<std::uint32_t>(m_frameIndex % aether::Swapchain::kMaxFramesInFlight);
			m_engine.GetServiceContainer().Get<RenderQueue>().SetWriteSlot(drawSlot);
			m_engine.GetServiceContainer().Get<RenderQueue>().Clear(drawSlot);
			m_engine.GetServiceContainer().Get<ShadowService>().PrepareWriteSlot(drawSlot);
			if (auto uiRenderer = m_engine.GetServiceContainer().TryGet<UIRenderer>())
			{
				uiRenderer->SetWriteSlot(drawSlot);
			}

			// ECS UI system: hit-test, drag, widget state (runs before OnGui).
			if (auto uiSystem = m_engine.GetServiceContainer().TryGet<ui::UiSystem>())
			{
				auto& uiWorld = m_engine.GetServiceContainer().Get<World>();
				auto& uiCtx = m_engine.GetServiceContainer().Get<ui::UiContext>();
				const auto extent = m_engine.GetServiceContainer().Get<Swapchain>().GetExtent();
				uiSystem->BeginFrame(uiWorld, m_engine.GetServiceContainer().Get<Input>(), uiCtx, extent, static_cast<float>(scaledDt));

				// Auto-render all ECS UI entities (panels, buttons, sliders, etc.).
				// Replaces explicit per-layer OnGui manual Draw* calls for ECS UI.
				if (auto uiRenderer = m_engine.GetServiceContainer().TryGet<UIRenderer>())
				{
					uiSystem->RenderAll(uiWorld, *uiRenderer, m_engine.GetServiceContainer().Get<Input>(), extent);
				}
			}

			// Layer game-logic update.
			{
				AE_PROFILE_ZONE();
				m_layers.UpdateAll(frameContext);
			}
			// Layer UI / overlay submission.
			{
				AE_PROFILE_ZONE();
				if (auto imgui = frameContext.TryGet<aether::ImguiSubsystem>())
				{
					imgui->BeginFrame(frameContext.services, static_cast<float>(deltaTime));
				}
				m_layers.GuiAll(frameContext);
			}

			if (auto uiSystem = m_engine.GetServiceContainer().TryGet<ui::UiSystem>())
			{
				auto& uiWorld = m_engine.GetServiceContainer().Get<World>();
				uiSystem->EndFrame(uiWorld);
			}

			// Flush ECS draws and build a frame packet.
			auto packet = m_engine.PrepareFrame(drawSlot, m_frameIndex);
			packet.elapsedTime = static_cast<float>(m_elapsedTimeSeconds);
			if (auto imgui = frameContext.TryGet<aether::ImguiSubsystem>())
			{
				imgui->CaptureFrame(packet.imgui);
			}

			// Hand the packet to the render thread.
			const auto submitStart = std::chrono::steady_clock::now();
			m_renderThread.SubmitFrame(std::move(packet));
			AE_PROFILE_PLOT("Frame/ChannelSubmitNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - submitStart).count()));
			AE_PROFILE_PLOT("Frame/GameThreadTotalNs", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - frameStartTime).count()));

			++m_frameIndex;
		}

		AE_INFO(LogCategory::App, "Application run loop exited.");
		m_renderThread.Stop();
		Logger::ClearFrameNumber();

		return 0;
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
