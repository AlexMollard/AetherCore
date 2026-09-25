#include "ScriptedSceneLayer.hpp"

#include <string>

#include "scripting/CSharpScriptingSubsystem.hpp"

#include "AetherCore.hpp"
#include "audio/AudioSubsystem.hpp"
#include "IEngineRuntime.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetSubsystem.hpp"
#include "material/MaterialAuthoring.hpp"
#include "camera/CameraManager.hpp"
#include "material/EffectManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "scene/ModelSpawn.hpp"
#include "passes/PostProcessStack.hpp"
#include "PlayState.hpp"
#include "platform/Input.hpp"
#include "scene/SceneSerializer.hpp"
#include "rendering/Renderer.hpp"
#include "gpu/GpuDevice.hpp"
#include "scene/World.hpp"
#include "physics/PhysicsSystem.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "systems/DayNightSystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "scene/SceneSubsystem.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/SettingsService.hpp"
#include "utils/Logger.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Profiler.hpp"

#ifdef AETHERCORE_EDITOR_APP
#	include "editor/EditorProjectContext.hpp"
#endif

namespace aether::app
{

	void ScriptedSceneLayer::DestroySceneEntities(LayerContext& context)
	{
		auto& world = context.Get<World>();
		for (const aether::Entity e: m_sceneCtx.sceneEntities)
		{
			world.Destroy(e);
		}
		m_sceneCtx.sceneEntities.clear();
		{
			auto& assets = context.Get<AssetManager>();
			for (auto& model: m_sceneCtx.loadedModels)
			{
				assets.ReleaseModelTextures(model);
			}
		}
		m_sceneCtx.loadedModels.clear();
		m_sceneCtx.loadedModelMap.clear();
		m_sceneCtx.meshCache.clear();

		context.Get<AssetManager>().GetMaterialAuthoring().ReleaseAll();

		context.Get<Renderer>().ClearPointLights();
		context.Get<Renderer>().ClearSpotLights();
	}

	void ScriptedSceneLayer::DoReload(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reloading entity scripts in-place");

		if (m_csharp != nullptr)
		{
			std::string buildError;
			if (!m_csharp->RebuildFromSource(buildError))
			{
				m_csharp->ReportScriptError("Script rebuild failed:\n" + buildError);
				return;
			}
		}

		if (auto* scriptSystem = context.TryGet<ScriptComponentSystem>())
		{
			scriptSystem->Invalidate(context.Get<World>());
		}

		if (m_csharp != nullptr && m_csharp->IsAvailable())
		{
			m_csharp->ClearErrors();
			m_csharp->LoadScripts();
		}

		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reload complete.");
	}

	void ScriptedSceneLayer::OnAttach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		m_csharp = context.TryGet<scripting::CSharpScriptingSubsystem>();

		context.Get<aether::AssetSubsystem>().InitializePipelineCache({
		        .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		        .depthFormat = context.Get<Swapchain>().GetDepthFormat(),
		        .descriptorHeapMappings = context.Get<BindlessManager>().GetDescriptorHeapMappings(),
		});

		{
			aether::effects::EffectDef plasma;
			plasma.templateDesc.shaderVfsPath = "shaders://plasma.spv";
			plasma.templateDesc.depthWriteEnable = true;
			plasma.defaultParams.tint = glm::vec4(1.0f, 0.3f, 0.8f, 1.0f);
			plasma.defaultParams.speed = 0.5f;
			plasma.defaultParams.scale = 2.0f;
			plasma.defaultParams.intensity = 0.8f;
			m_effectManager.Register("plasma", plasma);

			aether::effects::EffectDef molten;
			molten.templateDesc.shaderVfsPath = "shaders://molten.spv";
			molten.templateDesc.depthWriteEnable = true;
			molten.defaultParams.tint = glm::vec4(1.0f, 0.35f, 0.05f, 1.0f);
			molten.defaultParams.speed = 1.0f;
			molten.defaultParams.scale = 1.6f;
			molten.defaultParams.intensity = 1.2f;
			m_effectManager.Register("molten", molten);
		}

		m_sceneCtx.world = &context.Get<World>();
		m_sceneCtx.services = &context.services;
		m_sceneCtx.assets = &context.Get<AssetManager>();
		m_sceneCtx.cameras = &context.Get<CameraManager>();
		m_sceneCtx.renderer = &context.Get<Renderer>();
		m_sceneCtx.input = &context.Get<Input>();
		m_sceneCtx.effects = &m_effectManager;
		if (auto* assetsSub = context.TryGet<aether::AssetSubsystem>())
		{
			m_sceneCtx.uploadPool = assetsSub->GetUploadContext().GetCommandPool();
		}
		if (auto* dn = context.TryGet<aether::app::DayNightSystem>())
		{
			m_sceneCtx.dayNight = dn;
		}
		m_sceneCtx.primitives = &context.Get<PrimitiveMeshes>();
		if (auto* physSys = context.Get<World>().FindSystem("PhysicsSystem"))
		{
			m_sceneCtx.physics = dynamic_cast<aether::PhysicsSystem*>(physSys);
		}
		if (auto* phys2DSys = context.Get<World>().FindSystem("Physics2DSystem"))
		{
			m_sceneCtx.physics2D = dynamic_cast<aether::Physics2DSystem*>(phys2DSys);
		}
		m_sceneCtx.audio = context.TryGet<aether::audio::AudioSubsystem>();
		m_sceneCtx.engineRuntime = &context.Get<aether::IEngineRuntime>();
		if (auto* engine = context.TryGet<aether::AetherCore>())
		{
			m_sceneCtx.debugVertices = &engine->GetPendingDebugVertices();
		}

		context.services.Register<scripting::SceneContext>(m_sceneCtx);

		if (auto* assetDb = context.services.TryGet<AssetDatabase>())
		{
			assetDb->SetModelMeshResolver([this](const std::string& path, int primitiveIndex) -> const Mesh* { return m_sceneCtx.assets != nullptr ? scene::ResolveModelPrimitiveMesh(*m_sceneCtx.assets, m_sceneCtx, path, primitiveIndex) : nullptr; });
		}

		LoadStartupScene(context);
	}

	void ScriptedSceneLayer::LoadStartupScene(LayerContext& context)
	{
#ifdef AETHERCORE_EDITOR_APP
		const auto* project = context.TryGet<EditorProjectContext>();
		if (project != nullptr && !project->IsLoaded())
		{
			return;
		}
#endif

		const auto* settingsService = context.TryGet<aether::SettingsService>();
		if (settingsService == nullptr || settingsService->Get().app.startupScene.empty())
		{
			AE_WARN(LogCategory::App, "No startup scene configured (app.startupScene is empty) - booting an EMPTY world. Set it in ProjectSettings.toml / EngineSettings.toml.");
			return;
		}
		const std::string& sceneName = settingsService->Get().app.startupScene;
		scene::ApplySceneDeps deps{};
		deps.assets = m_sceneCtx.assets;
		deps.primitives = m_sceneCtx.primitives;
		deps.effectManager = m_sceneCtx.effects;
		deps.effectParams = m_sceneCtx.assets ? &m_sceneCtx.assets->GetEffectParamBuffer() : nullptr;
		deps.pipelines = m_sceneCtx.assets ? &m_sceneCtx.assets->GetPipelineCache() : nullptr;
		deps.sceneContext = &m_sceneCtx;
		deps.physics = m_sceneCtx.physics;
		deps.renderer = m_sceneCtx.renderer;
		if (const auto* bakeHook = context.TryGet<scene::ModelBakeHook>(); bakeHook != nullptr)
		{
			deps.ensureModelBaked = bakeHook->ensureBaked;
		}

		if (auto* scenes = context.TryGet<aether::SceneSubsystem>())
		{
			scenes->SetCurrentScene(sceneName);
		}

		if (const auto desc = scene::ReadSceneFile(sceneName))
		{
			scene::ApplyScene(*desc, context.Get<World>(), deps);
			AE_INFO(LogCategory::App, "Startup scene '{}' loaded ({} entities)", sceneName, desc->entities.size());
		}
		else if (m_sceneCtx.assets != nullptr)
		{
			AE_WARN(LogCategory::App, "Startup scene '{}' could not be read - attempting to auto-generate it from script content.", sceneName);
			const auto captured = scene::CaptureScene(context.Get<World>(), m_sceneCtx.assets->GetMaterialRegistry(), m_sceneCtx.assets->GetTextureRegistry(), m_sceneCtx.renderer);
			if (scene::SaveSceneFile(sceneName, captured))
			{
				AE_INFO(LogCategory::App, "Startup scene '{}' auto-generated from script content - commit resources/scenes/{}.scene.toml", sceneName, sceneName);
			}
			else
			{
				AE_ERROR(LogCategory::App, "Startup scene '{}' auto-generate FAILED - the world is empty.", sceneName);
			}
		}
		else
		{
			AE_ERROR(LogCategory::App, "Startup scene '{}' could not be read and no asset manager is available to auto-generate it - the world is empty.", sceneName);
		}
	}

	void ScriptedSceneLayer::OnDetach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		DestroySceneEntities(context);
		context.services.Unregister<scripting::SceneContext>();

		m_sceneCtx.defaultMaterialInitialized = false;
		m_sceneCtx.defaultMaterial = {};
		m_sceneCtx.meshCache.clear();
	}

	void ScriptedSceneLayer::OnUpdate(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		if (m_csharp != nullptr && m_csharp->HasReloadRequest())
		{
			// A live Play session is a SIMULATION of the saved/authored state - the same
			// reasoning AutosaveService.cpp's own Play guard already uses to refuse a
			// background write. DoReload() calls ScriptComponentSystem::Invalidate(),
			// which destroys and recreates every C# instance in the world; anything a
			// running script had cached about itself (FirstPersonPlayer's own camera
			// child, resolved once in OnAttach and never revisited) does not survive
			// that, so mid-Play a script that looked correctly wired the frame before
			// silently stops working the frame after - not from anything the player did,
			// from a dev-only F5/"Reload Scripts" shortcut nothing gates today. Dropped
			// outright rather than deferred to Stop: a reload firing as a surprise the
			// moment Play ends is its own kind of confusing.
			const auto* playState = context.TryGet<PlayState>();
			if (playState != nullptr && playState->IsPlaying())
			{
				m_csharp->ClearReloadRequest();
				AE_WARN(LogCategory::App, "ScriptedSceneLayer: script reload ignored while Play is running - Stop first.");
			}
			else
			{
				m_csharp->ClearReloadRequest();
				DoReload(context);
			}
		}
	}
} // namespace aether::app
