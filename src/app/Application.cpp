#include "Application.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	include <Windows.h>
#endif

#include "animation/AnimationSystem.hpp"
#include "animation/SpriteAnimationSystem.hpp"
#include "assets/AssetManager.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
// Dear ImGui is used by every tooling front end (editor AND launcher) but never by
#ifdef AETHERCORE_WITH_IMGUI
#	include "imgui/ImguiSubsystem.hpp"
#endif
#include "io/PlatformPaths.hpp"
#ifdef AETHERCORE_EDITOR_APP
#	include "utils/TomlConfig.hpp"
#endif
#include "physics/PhysicsSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "io/FileSystem.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/ShadowService.hpp"
#include "assets/AssetDatabase.hpp"
#include "scene/World.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "particles/ParticleSystem.hpp"
#include "scene/BehaviorSystem.hpp"
#include "scene/CameraSystem.hpp"
#include "camera/CameraManager.hpp"
#include "scene/LightSystem.hpp"
#ifdef AETHERCORE_SCENE_APP
#	include "systems/DayNightSystem.hpp"
#	include "systems/ScriptComponentSystem.hpp"
#	include "PlaySession.hpp"
#endif

namespace aether::app
{
	namespace
	{
		void SignalEditorReady()
		{
#ifdef _WIN32
			const std::string eventName = io::PlatformPaths::ReadEnvironmentVariable("AETHER_EDITOR_READY_EVENT");
			if (eventName.empty())
			{
				return;
			}
			if (const HANDLE eventHandle = OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName.c_str()))
			{
				SetEvent(eventHandle);
				CloseHandle(eventHandle);
				AE_INFO(LogCategory::App, "Editor startup readiness confirmed to Launcher.");
			}
			else
			{
				AE_WARN(LogCategory::App, "Could not signal Editor startup readiness (GetLastError={}).", GetLastError());
			}
#endif
		}

#ifdef AETHERCORE_EDITOR_APP
		FramebufferSize ResolveEditorBootWindow(int defaultWidth, int defaultHeight)
		{
			aether::TomlConfig state;
			(void) state.LoadFromPath(aether::io::PlatformPaths::GetUserConfigDir() / "EditorState.toml");
			const int savedW = static_cast<int>(state.GetFloat("editor.window_width", static_cast<float>(defaultWidth)));
			const int savedH = static_cast<int>(state.GetFloat("editor.window_height", static_cast<float>(defaultHeight)));
			const bool valid = savedW >= 640 && savedH >= 480;
			return {.width = valid ? savedW : defaultWidth, .height = valid ? savedH : defaultHeight};
		}
#endif

		aether::AetherCore::Config BuildConfigFromSettings(const aether::AetherCore::Config& baseConfig, const aether::EngineSettings& settings)
		{
			aether::AetherCore::Config cfg = baseConfig;
			cfg.enableVsync = settings.graphics.vsync;
			if (cfg.profile == aether::RuntimeProfile::UiShell)
			{
				return cfg;
			}
#ifdef AETHERCORE_EDITOR_APP
			const FramebufferSize boot = ResolveEditorBootWindow(settings.window.width, settings.window.height);
			cfg.width = boot.width;
			cfg.height = boot.height;
#else
			cfg.width = settings.window.width;
			cfg.height = settings.window.height;
#endif
			return cfg;
		}

		std::filesystem::path ResolveRuntimeProjectSettingsFile()
		{
#ifdef AETHERCORE_EDITOR_APP
			return {};
#else
			std::filesystem::path projectRoot;
			if (const std::string env = io::PlatformPaths::ReadEnvironmentVariable("AETHER_PROJECT_DIR"); !env.empty())
			{
				projectRoot = env;
			}
			if (projectRoot.empty())
			{
				const std::filesystem::path publishedSettings = io::PlatformPaths::GetExecutableDir() / "data" / "config" / "ProjectSettings.toml";
				std::error_code ec;
				if (std::filesystem::exists(publishedSettings, ec))
				{
					return publishedSettings;
				}
			}
#	ifdef AETHER_DEFAULT_PROJECT_DIR
			if (projectRoot.empty())
			{
				projectRoot = AETHER_DEFAULT_PROJECT_DIR;
			}
#	endif
			if (projectRoot.empty())
			{
				return {};
			}
			const std::filesystem::path candidate = projectRoot / "ProjectSettings.toml";
			std::error_code ec;
			return std::filesystem::exists(candidate, ec) ? candidate : std::filesystem::path{};
#endif
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
	      : Application(engineConfig, aether::EngineSettingsIO::LoadLayered(engineConfig.settingsFile, ResolveRuntimeProjectSettingsFile()))
	{
	}

	Application::Application(const aether::AetherCore::Config& engineConfig, const aether::LoadedEngineSettings& loaded)
	      : m_engine(BuildConfigFromSettings(engineConfig, loaded.values), loaded.values), m_settingsService(loaded.values, loaded.base, m_engine.GetServiceContainer())
	{
#ifdef AETHERCORE_WITH_IMGUI
		// (never inside AetherCore, which has zero knowledge of any UI toolkit) and
		auto imgui = std::make_unique<aether::ImguiSubsystem>();
		aether::ImguiSubsystem& imguiRef = *imgui;
		m_engine.SetUiOverlay(std::move(imgui));
		m_engine.GetServiceContainer().Register<aether::ImguiSubsystem>(imguiRef);
#endif

		AE_INFO(LogCategory::App, "Application created.");
	}

	Application::~Application()
	{
		m_settingsService.Save();

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
		m_engine.StopRenderThread();

		aether::coro::set_default_executor(nullptr);

		// (AETHERCORE_SCENE_APP undefined) never wires them, so it never links the
#ifdef AETHERCORE_SCENE_APP
		context.Get<World>().UnregisterSystem("ScriptComponentSystem");
		context.Get<World>().UnregisterSystem("CameraSystem");
		context.Get<World>().UnregisterSystem("LightSystem");
		context.Get<World>().UnregisterSystem("DayNightSystem");
		context.Get<World>().UnregisterSystem("SpriteAnimationSystem");
		context.Get<World>().UnregisterSystem("AnimationSystem");
		context.Get<World>().UnregisterSystem("PhysicsSystem");
		context.Get<World>().UnregisterSystem("Physics2DSystem");
#endif

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
		aether::coro::set_default_executor(&m_coroExecutor);

		// Start the engine-owned render thread early (also registers the
		m_engine.StartRenderThread();

		LayerContext attachContext{
		        .services = m_engine.GetServiceContainer(),
		        .deltaTimeSeconds = 0.0,
		        .frameIndex = 0,
		};

		{
			auto& services = attachContext.services;

			m_playState.SetMode(m_settingsService.Get().app.autoplay ? PlayState::Mode::Playing : PlayState::Mode::Editing);
			services.Register<PlayState>(m_playState);
			services.Register<aether::SettingsService>(m_settingsService);

#ifdef AETHERCORE_SCENE_APP
			{
				attachContext.Get<World>().RegisterSystem(std::make_unique<aether::AnimationSystem>());
				auto spriteAnimationSystem = std::make_unique<aether::SpriteAnimationSystem>(attachContext.Get<aether::SpriteAssetStore>());
				auto* spriteAnimationPtr = spriteAnimationSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(spriteAnimationSystem));
				services.Register<aether::SpriteAnimationSystem>(*spriteAnimationPtr);

				auto physicsSystem = std::make_unique<aether::PhysicsSystem>();
				auto* physicsPtr = physicsSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(physicsSystem));
				services.Register<aether::PhysicsSystem>(*physicsPtr);

				auto physics2DSystem = std::make_unique<aether::Physics2DSystem>();
				physics2DSystem->SetTileAssets(attachContext.TryGet<aether::TileAssetStore>());
				auto* physics2DPtr = physics2DSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(physics2DSystem));
				services.Register<aether::Physics2DSystem>(*physics2DPtr);

				auto dayNightSystem = std::make_unique<aether::app::DayNightSystem>();
				dayNightSystem->Init(*attachContext.TryGet<Renderer>());
				auto* dayNightPtr = dayNightSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(dayNightSystem));
				services.Register<aether::app::DayNightSystem>(*dayNightPtr);

				auto lightSystem = std::make_unique<aether::LightSystem>(*attachContext.TryGet<Renderer>());
				auto* lightPtr = lightSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(lightSystem));
				services.Register<aether::LightSystem>(*lightPtr);

				auto cameraSystem = std::make_unique<aether::CameraSystem>(*attachContext.TryGet<aether::CameraManager>());
				auto* cameraPtr = cameraSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(cameraSystem));
				services.Register<aether::CameraSystem>(*cameraPtr);

				attachContext.Get<World>().RegisterSystem(std::make_unique<BehaviorSystem>(attachContext.Get<AssetManager>()));

				auto scriptSystem = std::make_unique<ScriptComponentSystem>(services);
				auto* scriptPtr = scriptSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(scriptSystem));
				services.Register<ScriptComponentSystem>(*scriptPtr);

				// After scripts, so a burst a script queues this frame emits now.
				auto particleSystem = std::make_unique<aether::ParticleSystem>(attachContext.Get<AssetManager>().GetTextureRegistry());
				auto* particlePtr = particleSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(particleSystem));
				services.Register<aether::ParticleSystem>(*particlePtr);
			}
#endif
		}

		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		m_settingsService.ApplyAll();
		SignalEditorReady();

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
		        .elapsedTimeSeconds = 0.0,
		        .frameIndex = frameIndex,
		};
	}

	void Application::OnFrameBegin()
	{
		m_coroExecutor.drain();
	}

	double Application::GetTimeScale()
	{
		constexpr double kFastForwardScale = 10.0;
		const auto& input = m_engine.GetServiceContainer().Get<Input>();
		return input.IsKeyDown(Key::GraveAccent) ? kFastForwardScale : 1.0;
	}

	void Application::OnUpdate(double gameDt, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
		LayerContext ctx = MakeLayerContext(gameDt, frameIndex);

#ifdef AETHERCORE_SCENE_APP
		if (auto* assetDb = ctx.TryGet<AssetDatabase>())
		{
			assetDb->ResolveWorldMeshes(ctx.Get<World>());
		}

		// its worker thread, this polls it each frame and enters Playing when it's
		UpdatePlaySession(ctx);

		if (auto* cameraSystem = ctx.TryGet<aether::CameraSystem>())
		{
			cameraSystem->SetApplyMainCamera(m_playState.IsPlaying());
		}

		if (m_playState.IsPlaying())
		{
			ctx.Get<World>().UpdateSystems(static_cast<float>(gameDt));
		}
		else
		{
			// Edit-mode previews mirror play mode: every system participates in
			// every scene (Unity-style); each skips cheaply when it has nothing.
			World& editWorld = ctx.Get<World>();
			if (auto* spriteAnimations = ctx.TryGet<aether::SpriteAnimationSystem>())
			{
				spriteAnimations->UpdatePreview(editWorld, static_cast<float>(gameDt));
			}
			// Preview particle emitters while editing so effects are authorable.
			if (auto* particles = ctx.TryGet<aether::ParticleSystem>())
			{
				particles->Update(editWorld, static_cast<float>(gameDt));
			}
			if (auto* physics = ctx.TryGet<aether::PhysicsSystem>())
			{
				physics->FlushPendingOnly(editWorld);
			}
			if (auto* physics2D = ctx.TryGet<aether::Physics2DSystem>())
			{
				physics2D->FlushPendingOnly(editWorld);
			}
			if (auto* dayNight = ctx.TryGet<aether::app::DayNightSystem>())
			{
				dayNight->Update(editWorld, 0.0f);
			}
			if (auto* lights = ctx.TryGet<aether::LightSystem>())
			{
				lights->Update(editWorld, 0.0f);
			}
			if (auto* cameras = ctx.TryGet<aether::CameraSystem>())
			{
				cameras->Update(editWorld, 0.0f);
			}
		}
#endif
		m_layers.UpdateAll(ctx);
	}

	void Application::OnBuildUI(double gameDt, std::uint64_t frameIndex)
	{
		AE_PROFILE_ZONE();
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
