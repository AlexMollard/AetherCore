#include "Application.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>

#include "animation/AnimationSystem.hpp"
#include "assets/AssetManager.hpp"
// Dear ImGui is used by every tooling front end (editor AND launcher) but never by
// the shipped GameRuntime, so it is gated on AETHERCORE_WITH_IMGUI (defined by both
// tool targets) rather than AETHERCORE_EDITOR_APP.
#ifdef AETHERCORE_WITH_IMGUI
#	include "imgui/ImguiSubsystem.hpp"
#endif
#ifdef AETHERCORE_EDITOR_APP
#	include "io/PlatformPaths.hpp"
#	include "utils/TomlConfig.hpp"
#endif
#include "physics/PhysicsSystem.hpp"
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
#include "scene/BehaviorSystem.hpp"
#include "scene/CameraSystem.hpp"
#include "camera/CameraManager.hpp"
#include "scene/LightSystem.hpp"
// App-level scene systems + the play-session pump are scene-app only (the launcher
// neither compiles nor links them - see the AETHERCORE_SCENE_APP blocks below).
#ifdef AETHERCORE_SCENE_APP
#	include "systems/DayNightSystem.hpp"
#	include "systems/ScriptComponentSystem.hpp"
#	include "PlaySession.hpp"
#endif

namespace aether::app
{
	namespace
	{
#ifdef AETHERCORE_EDITOR_APP
		// Editor boot window size: the project launcher always opens at a fixed
		// 1920x1080; a project reopened at boot (open_last) uses the user's last
		// editor window size, defaulting to the configured resolution. Decided up
		// front from EditorState so the window opens at the right size with no
		// visible resize. Runtime launcher<->editor transitions + size persistence
		// live in DebugLayer; this only picks the initial size. Mirrors
		// EditorProjectManager::LoadSettings' auto-open condition (open_last +
		// launcher.current.path).
		FramebufferSize ResolveEditorBootWindow(int defaultWidth, int defaultHeight)
		{
			// The editor window always opens at the user's editor size (saved, or the
			// configured default). The launcher no longer drives the OS window size, so
			// there is no launcher-vs-editor boot-size branch.
			aether::TomlConfig state;
			state.LoadFromPath(aether::io::PlatformPaths::GetUserConfigDir() / "EditorState.toml");
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

		// GameRuntime: resolve the project's ProjectSettings.toml so the settings
		// cascade gets its layer 3 (project overrides - app.startupScene lives
		// there). Mirrors FileSystem's project-root resolution: the AETHER_PROJECT_DIR
		// env override first, then the build-tree default project. Without this the
		// build-tree runtime boots with the shipped template's startupScene="" and
		// silently renders an empty world. The editor returns empty here - its
		// project manager owns the layer and re-resolves it on project open. A
		// published game normally has neither path (publish BAKES the project layer
		// into the shipped EngineSettings.toml); the empty path skips the layer.
		std::filesystem::path ResolveRuntimeProjectSettingsFile()
		{
#ifdef AETHERCORE_EDITOR_APP
			return {};
#else
			std::filesystem::path projectRoot;
			if (const char* env = std::getenv("AETHER_PROJECT_DIR"); env != nullptr && *env != '\0')
			{
				projectRoot = env;
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
			std::filesystem::path candidate = projectRoot / "ProjectSettings.toml";
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
		// Dear ImGui tooling is used by the editor and the launcher: construct it here
		// (never inside AetherCore, which has zero knowledge of any UI toolkit) and
		// install it as the engine's optional UI overlay. GameRuntime never compiles
		// this block, so it neither links nor initializes imgui - see SetUiOverlay's
		// doc comment in AetherCore.hpp.
		auto imgui = std::make_unique<aether::ImguiSubsystem>();
		aether::ImguiSubsystem& imguiRef = *imgui;
		m_engine.SetUiOverlay(std::move(imgui)); // calls imguiRef.Init(...)
		m_engine.GetServiceContainer().Register<aether::ImguiSubsystem>(imguiRef);
#endif

		AE_INFO(LogCategory::App, "Application created.");
	}

	Application::~Application()
	{
		// Persist user setting changes before any teardown. The VFS and engine
		// services are still alive at this point (m_engine is destroyed after this
		// body runs). The service is the single source of truth, so no runtime
		// state needs to be synced back first.
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
		// which destroy GPU-referenced resources. StopRenderThread() does both.
		m_engine.StopRenderThread();

		// Reset the default executor so no more continuations are dispatched.
		aether::coro::set_default_executor(nullptr);

		// Unregister engine-level systems before detaching layers. Scene systems are
		// only wired in a scene app (editor / game runtime) - see Run. The Launcher
		// (AETHERCORE_SCENE_APP undefined) never wires them, so it never links the
		// app-level scene systems (ScriptComponentSystem's CoreCLR host, DayNightSystem).
#ifdef AETHERCORE_SCENE_APP
		context.Get<World>().UnregisterSystem("ScriptComponentSystem");
		context.Get<World>().UnregisterSystem("CameraSystem");
		context.Get<World>().UnregisterSystem("LightSystem");
		context.Get<World>().UnregisterSystem("DayNightSystem");
		context.Get<World>().UnregisterSystem("AnimationSystem");
		context.Get<World>().UnregisterSystem("PhysicsSystem");
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
			m_playState.SetMode(m_settingsService.Get().app.autoplay ? PlayState::Mode::Playing : PlayState::Mode::Editing);
			services.Register<PlayState>(m_playState);
			// Single source of truth for settings: layers read through it, the
			// settings/scene UIs edit through it (live-applying changes), and it
			// persists the user delta on shutdown.
			services.Register<aether::SettingsService>(m_settingsService);

			// ECS scene systems belong to a scene app (editor / game runtime). The
			// Launcher (a tooling front end built without AETHERCORE_SCENE_APP) has no
			// scene, so it neither wires nor links them - crucially keeping the
			// CoreCLR host (ScriptComponentSystem) and DayNightSystem out of its binary.
			// The engine mirrors this at runtime via RuntimeProfile::UiShell.
#ifdef AETHERCORE_SCENE_APP
			{
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

				// Entity cameras -> CameraManager backing pool, every frame (see
				// CameraSystem). Registered for the edit-mode dt=0 call below.
				auto cameraSystem = std::make_unique<aether::CameraSystem>(*attachContext.TryGet<aether::CameraManager>());
				auto cameraPtr = cameraSystem.get();
				attachContext.Get<World>().RegisterSystem(std::move(cameraSystem));
				services.Register<aether::CameraSystem>(*cameraPtr);

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
#endif
		}

		m_layers.AttachAll(attachContext);
		m_layersAttached = true;

		// Push settings that subsystems don't consume at init (FXAA, target FPS) so
		// file values actually take effect; VSync/resolution were already applied
		// during device/window setup and no-op here.
		m_settingsService.ApplyAll();

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

		// Scene simulation (asset resolve, play-session pump, per-mode system ticks)
		// belongs to a scene app. The Launcher (no AETHERCORE_SCENE_APP) has no scene
		// or systems and only drives its layers below - this also keeps PlaySession /
		// ScriptComponentSystem (CoreCLR) and DayNightSystem out of its binary.
#ifdef AETHERCORE_SCENE_APP
		// Keep every mesh pointer resolved from its asset id - back-fills the id on
		// first sight and re-points meshes whose asset was hot-reloaded - before
		// systems, UI or rendering read them. Runs in both play and edit modes.
		if (auto* assetDb = ctx.TryGet<AssetDatabase>())
		{
			assetDb->ResolveWorldMeshes(ctx.Get<World>());
		}

		// Pump the async play transition: while a Play-triggered script build runs on
		// its worker thread, this polls it each frame and enters Playing when it's
		// done (or returns to Editing on error). A no-op outside the Compiling state,
		// so it costs nothing in edit or play mode and in the shipped runtime.
		UpdatePlaySession(ctx);

		// The scene's main camera only owns the render view while Playing; in edit
		// mode the free-look editor camera owns it (CameraSystem otherwise reclaims
		// the view every frame, making the editor camera uncontrollable).
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
			// Entity cameras sync their backing pool cameras while paused too, so
			// frustum gizmos and the look-through preview track edits live.
			if (auto* cameras = ctx.TryGet<aether::CameraSystem>())
			{
				cameras->Update(ctx.Get<World>(), 0.0f);
			}
		}
#endif
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
