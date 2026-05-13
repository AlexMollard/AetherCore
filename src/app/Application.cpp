#include "Application.hpp"

#include <chrono>

#include "animation/AnimationSystem.hpp"
#include "FileSystem.hpp"
#include "ui/UIRenderer.hpp"
#include "ui/UiSystem.hpp"
#include "ui/UiWorld.hpp"
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
		INFO(LogCategory::App, "Application created.");
	}

	Application::~Application()
	{
		if (!m_layersAttached)
		{
			VERBOSE(LogCategory::App, "Application destroyed before layers were attached.");
			return;
		}

		LayerContext context{
			.services = m_engine.GetServiceContainer(),
			.deltaTimeSeconds = 0.0,
			.frameIndex = m_frameIndex,
		};

		m_renderThread.Stop();
		m_engine.WaitIdle();

		// Reset the default executor so no more continuations are dispatched.
		aether::coro::set_default_executor(nullptr);

		// Unregister engine-level systems before detaching layers.
		context.Get<World>().UnregisterSystem("DayNightSystem");
		context.Get<World>().UnregisterSystem("AnimationSystem");

		m_layers.DetachAll(context);
		m_imguiRenderer.Shutdown(m_engine.GetServiceContainer());
		// Engine UIRenderer shutdown is handled by AetherCore.
		INFO(LogCategory::App, "Application shutdown complete.");
	}

	void Application::PushLayer(std::unique_ptr<AppLayer> layer)
	{
		m_layers.Push(std::move(layer));
		VERBOSE(LogCategory::App, "Layer pushed to stack.");
	}

	int Application::Run()
	{
		INFO(LogCategory::App, "Application run loop starting.");

		const auto shaderFiles = io::FileSystem::Glob("shaders://**/*.slang.spv");
		if (shaderFiles.empty())
		{
			WARN(LogCategory::FileSystem, "No compiled shader files found via shaders://**/*.slang.spv");
		}
		else
		{
			INFO(LogCategory::FileSystem, "Discovered {} compiled shader file(s).", shaderFiles.size());
			for (const auto& shaderFile: shaderFiles)
			{
				VERBOSE(LogCategory::FileSystem, "Shader asset: shaders://{}", shaderFile);
			}
		}

		Logger::SetFrameNumber(0);
		m_imguiRenderer.Init(m_engine.GetServiceContainer(), m_engine.GetServiceContainer().Get<Window>().GetHandle());
		m_engine.SetSwapchainRecreatedCallback([this](aether::AetherCore& e) { m_imguiRenderer.ReregisterPass(e.GetServiceContainer()); });

		// Set the coroutine default executor - all cross-thread continuation
		// resumptions (e.g. I/O thread → game thread) go through this queue
		// and are drained at the top of each frame.
		// This is set up early so that any async operations during loading
		// dispatch correctly.
		aether::coro::set_default_executor(&m_coroExecutor);

		// Register the shared LoadingManager so that the LoadingLayer,
		// SandboxGameSystem, and any other loading participant can read
		// progress or push tasks through the same instance.
		m_engine.GetServiceContainer().Register<LoadingManager>(m_loadingManager);

		// Start the dedicated render thread early so that loading-screen
		// frames can be submitted while assets load incrementally.
		m_renderThread.Start(m_engine);

		LayerContext attachContext{
			.services = m_engine.GetServiceContainer(),
			.deltaTimeSeconds = 0.0,
			.frameIndex = 0,
		};

		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		// Register engine-level systems.
		attachContext.Get<World>().RegisterSystem(std::make_unique<aether::AnimationSystem>());
		auto dayNightSystem = std::make_unique<aether::app::DayNightSystem>();
		dayNightSystem->Init(*attachContext.TryGet<Renderer>());
		attachContext.Get<World>().RegisterSystem(std::move(dayNightSystem));

		if (m_settings.app.targetFps > 0.0f)
		{
			INFO(LogCategory::App, "Using settings TargetFPS={}.", m_settings.app.targetFps);
			m_framePacer.SetTargetFps(m_settings.app.targetFps);
		}
		else if (m_settings.graphics.vsync)
		{
			const int refreshRate = m_engine.GetServiceContainer().Get<Window>().GetDisplayRefreshRate();
			if (refreshRate > 0)
			{
				INFO(LogCategory::App, "Display refresh rate: {} Hz - setting frame pacer target.", refreshRate);
				m_framePacer.SetTargetFps(static_cast<float>(refreshRate));
			}
			else
			{
				WARN(LogCategory::App, "Could not query display refresh rate - frame pacer running uncapped.");
			}
		}
		else
		{
			INFO(LogCategory::App, "VSync is off and TargetFPS is 0 - frame pacer running uncapped.");
		}

		auto previousFrameTime = std::chrono::steady_clock::now();

		while (!m_engine.ShouldClose())
		{
			AE_PROFILE_ZONE_N("Frame");

			m_framePacer.Wait();

			// Resume any coroutines whose async I/O completed on the background
			// thread since the last frame.
			m_coroExecutor.drain();

			Logger::SetFrameNumber(m_frameIndex);

			m_engine.PumpEvents();

			const auto currentFrameTime = std::chrono::steady_clock::now();
			const auto deltaTime = std::chrono::duration<double>(currentFrameTime - previousFrameTime).count();
			previousFrameTime = currentFrameTime;

			// Update engine-level per-frame systems (input + camera).
			// Camera uses unscaled dt so it stays controllable during fast-forward.
			m_engine.Tick(static_cast<float>(deltaTime));

			// Fast-forward: hold GraveAccent (` / ~) to multiply game speed.
			constexpr double kFastForwardScale = 10.0;
			const auto& input = m_engine.GetServiceContainer().Get<Input>();
			const double timeScale = input.IsKeyDown(Key::GraveAccent) ? kFastForwardScale : 1.0;
			const double scaledDt = deltaTime * timeScale;

			LayerContext frameContext{
				.services = m_engine.GetServiceContainer(),
				.deltaTimeSeconds = scaledDt,
				.frameIndex = m_frameIndex,
			};

			// Update ECS systems (game logic) with scaled dt.
			{
				AE_PROFILE_ZONE_N("WorldSystems");
				frameContext.Get<World>().UpdateSystems(static_cast<float>(scaledDt));
			}

			// Compute the CPU double-buffer write slot for this frame.
			const auto drawSlot = static_cast<std::uint32_t>(m_frameIndex % aether::Swapchain::kMaxFramesInFlight);
			m_engine.GetServiceContainer().Get<RenderQueue>().SetWriteSlot(drawSlot);
			m_engine.GetServiceContainer().Get<RenderQueue>().Clear(drawSlot);
			m_engine.GetServiceContainer().Get<ShadowService>().PrepareWriteSlot(drawSlot);
			if (auto* uiRenderer = m_engine.GetServiceContainer().TryGet<UIRenderer>())
			{
				uiRenderer->SetWriteSlot(drawSlot);
			}
			m_imguiRenderer.SetWriteSlot(drawSlot);

			// ECS UI system: hit-test, drag, widget state (runs before OnGui).
			if (auto* uiSystem = m_engine.GetServiceContainer().TryGet<ui::UiSystem>())
			{
				auto& uiWorld = m_engine.GetServiceContainer().Get<ui::UiWorld>();
				auto& uiCtx = m_engine.GetServiceContainer().Get<ui::UiContext>();
				uiSystem->BeginFrame(uiWorld, m_engine.GetServiceContainer().Get<Input>(), uiCtx, m_engine.GetServiceContainer().Get<Swapchain>().GetExtent(), static_cast<float>(scaledDt));
			}

			// ImGui new frame.
			m_imguiRenderer.BeginFrame();

			// Layer game-logic update.
			{
				AE_PROFILE_ZONE_N("LayerUpdate");
				m_layers.UpdateAll(frameContext);
			}
			// Layer UI / overlay submission.
			{
				AE_PROFILE_ZONE_N("LayerGui");
				m_layers.GuiAll(frameContext);
			}

			if (auto* uiSystem = m_engine.GetServiceContainer().TryGet<ui::UiSystem>())
			{
				auto& uiWorld = m_engine.GetServiceContainer().Get<ui::UiWorld>();
				auto& uiCtx = m_engine.GetServiceContainer().Get<ui::UiContext>();
				uiSystem->EndFrame(uiWorld, uiCtx);
			}

			// Snapshot ImGui draw data.
			m_imguiRenderer.SnapshotFrame();

			// Render secondary OS windows.
			m_imguiRenderer.RenderPlatformWindows();

			// Flush ECS draws and build a frame packet.
			auto packet = m_engine.PrepareFrame(drawSlot, m_frameIndex);

			// Hand the packet to the render thread.
			m_renderThread.SubmitFrame(std::move(packet));

			AE_PROFILE_FRAME;
			++m_frameIndex;
		}

		INFO(LogCategory::App, "Application run loop exited.");
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
