#include "Application.hpp"

#include <chrono>

#include "FileSystem.hpp"
#include "Logger.hpp"
#include "AnimationSystem.hpp"
#include "systems/DayNightSystem.hpp"

namespace aether::app
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		constexpr std::string_view kUiFontPath = "assets://fonts/Roboto-Regular.ttf";
	}

	void AppLayer::OnAttach(LayerContext& context)
	{
		(void)context;
	}

	void AppLayer::OnDetach(LayerContext& context)
	{
		(void)context;
	}

	void AppLayer::OnUpdate(LayerContext& context)
	{
		(void)context;
	}

	void AppLayer::OnGui(LayerContext& context)
	{
		(void)context;
	}

	Application::Application(const aether::AetherCore::Config& engineConfig)
		: m_engine(engineConfig)
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
		// resources (pipelines, buffers, etc.) that may still be referenced by the GPU.
		m_engine.WaitIdle();

		// Unregister engine-level systems before detaching layers.
		context.world->UnregisterSystem("DayNightSystem");
		context.world->UnregisterSystem("AnimationSystem");

		// OnExit:
		// We call DetachAll() here to detach all layers before the application is destroyed,
		// This can be thought of like the onDestroy() function in unity or something like that,
		// where you can do cleanup of game objects and such, but the actual application is still running until this destructor returns and the application is destroyed
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

		// Testing the VFS, normally loading shader files would be done in a pipeline creation inside a render graph but im not upto that yet
		const auto shaderFiles = io::FileSystem::Glob("shaders://**/*.slang.spv");
		if (shaderFiles.empty())
		{
			WARN(LogCategory::FileSystem, "No compiled shader files found via shaders://**/*.slang.spv");
		}
		else
		{
			INFO(LogCategory::FileSystem, "Discovered {} compiled shader file(s).", shaderFiles.size());
			for (const auto& shaderFile : shaderFiles)
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
		// This can be thought of like the onStart() function in unity or something like that, 
		// where you can do initialization of game objects and such, but the actual game loop starts after this function returns and the main loop starts
		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		// Register engine-level systems.
		attachContext.world->RegisterSystem(std::make_unique<aether::AnimationSystem>());
		auto dayNightSystem = std::make_unique<aether::app::DayNightSystem>();
		dayNightSystem->Init(*attachContext.renderer);
		attachContext.world->RegisterSystem(std::move(dayNightSystem));

		auto previousFrameTime = Clock::now();
		while (!m_engine.ShouldClose())
		{
			Logger::SetFrameNumber(m_frameIndex);

			// Pump event sounds funny but it just means we are polling for events and such, so we call PumpEvents() here to poll for events and such before we do any updating or rendering
			// So like input events or window events and such
			m_engine.PumpEvents();

			const auto currentFrameTime = Clock::now();
			const auto deltaTime = std::chrono::duration<double>(currentFrameTime - previousFrameTime).count();
			previousFrameTime = currentFrameTime;

			// Per frame context:
			// A helper struct with useful objects and info you can use in your layers for the current frame.
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

			// Update engine-level per-frame systems before layers run.
			m_engine.Tick(static_cast<float>(deltaTime));

			// Update ECS systems (game logic).
			frameContext.world->UpdateSystems(static_cast<float>(deltaTime));

			// Update:
			// Game logic and such should be updated in the OnUpdate() function of the layers, so we call UpdateAll() here to update all layers
			m_layers.UpdateAll(frameContext);

			// Render:
			// World rendering is handled implicitly by the engine's frame passes.
			// GuiAll() is reserved for any explicit overlay/UI work layers want to submit.
			m_engine.BeginFrame();
			m_layers.GuiAll(frameContext);
			m_engine.EndFrame();

			++m_frameIndex;
		}

		INFO(LogCategory::App, "Application run loop exited.");
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
}