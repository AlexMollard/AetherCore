#include "Application.hpp"

#include <chrono>

#include "AnimationSystem.hpp"
#include "FileSystem.hpp"
#include "Logger.hpp"
#include "Profiler.hpp"
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
	      : m_settings(aether::EngineSettingsIO::LoadOrCreate(engineConfig.settingsFile)), m_engine(BuildConfigFromSettings(engineConfig, m_settings))
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
			.engine = m_engine,
			.deltaTimeSeconds = 0.0,
			.frameIndex = m_frameIndex,
			.scene = &m_engine.GetScene(),
			.world = &m_engine.GetWorld(),
			.input = &m_engine.GetInput(),
			.cameras = &m_engine.GetCameraManager(),
			.renderer = &m_engine.GetRenderer(),
			.assets = &m_engine.GetAssets(),
			.ui = &m_uiRenderer,
		};

		// Wait for the GPU to finish all in-flight work before tearing down app-layer
		// resources (pipelines, buffers, etc.) that may still be referenced by the
		// GPU.
		m_renderThread.Stop();
		m_engine.WaitIdle();

		// Unregister engine-level systems before detaching layers.
		context.world->UnregisterSystem("DayNightSystem");
		context.world->UnregisterSystem("AnimationSystem");

		// OnExit:
		// We call DetachAll() here to detach all layers before the application is
		// destroyed, This can be thought of like the onDestroy() function in unity or
		// something like that, where you can do cleanup of game objects and such, but
		// the actual application is still running until this destructor returns and
		// the application is destroyed
		m_layers.DetachAll(context);
		m_uiRenderer.Shutdown(m_engine);
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

		// Testing the VFS, normally loading shader files would be done in a pipeline
		// creation inside a render graph but im not upto that yet
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
		m_uiRenderer.Init(m_engine, kUiFontPath, "AppUI");

		LayerContext attachContext{
			.engine = m_engine,
			.deltaTimeSeconds = 0.0,
			.frameIndex = 0,
			.scene = &m_engine.GetScene(),
			.world = &m_engine.GetWorld(),
			.input = &m_engine.GetInput(),
			.cameras = &m_engine.GetCameraManager(),
			.renderer = &m_engine.GetRenderer(),
			.assets = &m_engine.GetAssets(),
			.ui = &m_uiRenderer,
		};

		// Startup:
		// We call AttachAll() here to attach all layers before the main loop starts,
		// This can be thought of like the onStart() function in unity or something
		// like that, where you can do initialization of game objects and such, but
		// the actual game loop starts after this function returns and the main loop
		// starts
		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		// Register engine-level systems.
		attachContext.world->RegisterSystem(std::make_unique<aether::AnimationSystem>());
		auto dayNightSystem = std::make_unique<aether::app::DayNightSystem>();
		dayNightSystem->Init(*attachContext.renderer);
		attachContext.world->RegisterSystem(std::move(dayNightSystem));

		// Start the dedicated render thread. All Vulkan submission work runs there.
		m_renderThread.Start(m_engine);

		if (m_settings.app.targetFps > 0.0f)
		{
			INFO(LogCategory::App, "Using settings TargetFPS={}.", m_settings.app.targetFps);
			m_framePacer.SetTargetFps(m_settings.app.targetFps);
		}
		else if (m_settings.graphics.vsync)
		{
			// Auto policy with VSync on: match the frame pacer to display refresh.
			const int refreshRate = m_engine.GetWindow().GetDisplayRefreshRate();
			if (refreshRate > 0)
			{
				INFO(LogCategory::App, "Display refresh rate: {} Hz — setting frame pacer target.", refreshRate);
				m_framePacer.SetTargetFps(static_cast<float>(refreshRate));
			}
			else
			{
				WARN(LogCategory::App, "Could not query display refresh rate — frame pacer running uncapped.");
			}
		}
		else
		{
			INFO(LogCategory::App, "VSync is off and TargetFPS is 0 — frame pacer running uncapped.");
		}

		auto previousFrameTime = std::chrono::steady_clock::now();

		while (!m_engine.ShouldClose())
		{
			AE_PROFILE_ZONE_N("Frame");

			// Pace to the target FPS.  Sleeps the game thread (coarse) then spins (fine)
			// until the next frame deadline.  Placing this at the top of the loop — before
			// deltaTime is measured — means:
			//   a) deltaTime is accurate (it includes the sleep).
			//   b) SubmitFrame() below will not stall: the render thread has had ~targetDuration
			//      to finish the previous frame before we ask it to accept the next one.
			m_framePacer.Wait();

			Logger::SetFrameNumber(m_frameIndex);

			m_engine.PumpEvents();

			const auto currentFrameTime = std::chrono::steady_clock::now();
			const auto deltaTime = std::chrono::duration<double>(currentFrameTime - previousFrameTime).count();
			previousFrameTime = currentFrameTime;

			LayerContext frameContext{
				.engine = m_engine,
				.deltaTimeSeconds = deltaTime,
				.frameIndex = m_frameIndex,
				.scene = &m_engine.GetScene(),
				.world = &m_engine.GetWorld(),
				.input = &m_engine.GetInput(),
				.cameras = &m_engine.GetCameraManager(),
				.renderer = &m_engine.GetRenderer(),
				.assets = &m_engine.GetAssets(),
				.ui = &m_uiRenderer,
			};

			// Update engine-level per-frame systems (camera, input).
			m_engine.Tick(static_cast<float>(deltaTime));

			// Update ECS systems (game logic).
			{
				AE_PROFILE_ZONE_N("WorldSystems");
				frameContext.world->UpdateSystems(static_cast<float>(deltaTime));
			}

			// Layer game-logic update.
			{
				AE_PROFILE_ZONE_N("LayerUpdate");
				m_layers.UpdateAll(frameContext);
			}

			// Compute the CPU double-buffer write slot for this frame and tell the
			// UI renderers so DrawText / DrawRect calls land in the correct slot.
			const auto drawSlot = static_cast<std::uint32_t>(m_frameIndex % aether::Swapchain::kMaxFramesInFlight);
			m_uiRenderer.SetWriteSlot(drawSlot);

			// Layer UI / overlay submission (writes into the double-buffered slot).
			{
				AE_PROFILE_ZONE_N("LayerGui");
				m_layers.GuiAll(frameContext);
			}

			// Flush ECS draws into the render queue slot and build a frame packet
			// from the current camera / lighting snapshot.
			auto packet = m_engine.PrepareFrame(drawSlot, m_frameIndex);

			// Hand the packet to the render thread. Blocks for only ~microseconds
			// (thread wake latency) until the render thread picks it up.
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
