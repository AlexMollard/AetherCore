#include "ScriptedSceneLayer.hpp"

#include <string>

#include "scripting/CSharpScriptingSubsystem.hpp"

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
#include "platform/Input.hpp"
#include "scene/SceneSerializer.hpp"
#include "rendering/Renderer.hpp"
#include "gpu/GpuDevice.hpp"
#include "scene/World.hpp"
#include "physics/PhysicsSystem.hpp"
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
	// -- Helpers ---------------------------------------------------------------

	void ScriptedSceneLayer::DestroySceneEntities(LayerContext& context)
	{
		auto& world = context.Get<World>();
		for (aether::Entity e: m_sceneCtx.sceneEntities)
		{
			world.Destroy(e);
		}
		m_sceneCtx.sceneEntities.clear();
		// Release each cached model's loader-held texture refs before dropping the
		// models (spawned entities already released their material->texture refs on
		// destruction above); the textures free once no ref remains.
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

		// Drop authored-material bookkeeping so reloads don't accumulate ids
		// (entities' material handles were already released by their destruction).
		context.Get<AssetManager>().GetMaterialAuthoring().ReleaseAll();

		// Clear script-created lights so reload doesn't stack duplicates.
		context.Get<Renderer>().ClearPointLights();
		context.Get<Renderer>().ClearSpotLights();
	}

	void ScriptedSceneLayer::DoReload(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reloading entity scripts in-place");

		// Dev: rebuild the game scripts from source first (no-op in a packaged
		// build). On a build failure keep the running scene intact and surface the
		// compiler output instead of tearing everything down for a reload.
		if (m_csharp != nullptr)
		{
			std::string buildError;
			if (!m_csharp->RebuildFromSource(buildError))
			{
				m_csharp->ReportScriptError("Script rebuild failed:\n" + buildError);
				return;
			}
		}

		// Tear down every live managed instance before reloading the assembly so
		// the collectible load context can unload (no live GCHandles remain).
		// Keep the authored world intact: ScriptComponent paths, overrides and
		// unsaved Entity field assignments belong to the editor scene state.
		if (auto* scriptSystem = context.TryGet<ScriptComponentSystem>())
		{
			scriptSystem->Invalidate(context.Get<World>());
		}

		// Reload the game-scripts assembly so edits take effect; instances
		// re-create from the fresh types on the next play tick.
		if (m_csharp != nullptr && m_csharp->IsAvailable())
		{
			m_csharp->ClearErrors();
			m_csharp->LoadScripts();
		}

		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reload complete.");
	}

	// -- AppLayer overrides ----------------------------------------------------

	void ScriptedSceneLayer::OnAttach(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		// C# is optional: a missing runtime just means no entity-script behavior;
		// the scene still boots and renders.
		m_csharp = context.TryGet<scripting::CSharpScriptingSubsystem>();

		// Initialize the content-addressed pipeline cache with the frame-graph-
		// constant formats + bindless heap mappings (replaces the old default
		// pipeline; every entity's pipeline now resolves through the cache).
		context.Get<aether::AssetSubsystem>().InitializePipelineCache({
		        .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		        .depthFormat = context.Get<Swapchain>().GetDepthFormat(),
		        .descriptorHeapMappings = context.Get<BindlessManager>().GetDescriptorHeapMappings(),
		});

		// Register effects so script can use set_entity_effect(). The pipeline is
		// resolved lazily by PipelineCache; the manager only holds the template +
		// default params. Parity defaults copied from the old plasmaMat.
		{
			aether::effects::EffectDef plasma;
			plasma.templateDesc.shaderVfsPath = "shaders://plasma.spv";
			plasma.templateDesc.depthWriteEnable = true;
			plasma.defaultParams.tint = glm::vec4(1.0f, 0.3f, 0.8f, 1.0f); // pink
			plasma.defaultParams.speed = 0.5f;
			plasma.defaultParams.scale = 2.0f;
			plasma.defaultParams.intensity = 0.8f;
			m_effectManager.Register("plasma", plasma);

			// Molten: a dark obsidian crust broken by flowing white-hot veins - a
			// visual counterpoint to plasma over the same per-entity EffectParams.
			aether::effects::EffectDef molten;
			molten.templateDesc.shaderVfsPath = "shaders://molten.spv";
			molten.templateDesc.depthWriteEnable = true;
			molten.defaultParams.tint = glm::vec4(1.0f, 0.35f, 0.05f, 1.0f); // ember orange
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
		if (auto assetsSub = context.TryGet<aether::AssetSubsystem>())
		{
			m_sceneCtx.uploadPool = assetsSub->GetUploadContext().GetCommandPool();
		}
		if (auto dn = context.TryGet<aether::app::DayNightSystem>())
		{
			m_sceneCtx.dayNight = dn;
		}
		m_sceneCtx.primitives = &context.Get<PrimitiveMeshes>();
		if (auto physSys = context.Get<World>().FindSystem("PhysicsSystem"))
		{
			m_sceneCtx.physics = static_cast<aether::PhysicsSystem*>(physSys);
		}
		m_sceneCtx.engineRuntime = &context.Get<aether::IEngineRuntime>();

		// Scene tooling (the debug-UI serializer) reaches the model cache, the
		// effect manager and sceneEntities through the service container.
		context.services.Register<scripting::SceneContext>(m_sceneCtx);

		// The engine-side asset database resolves built-in primitives itself; give
		// it a way to resolve glTF model primitives through this layer's model cache.
		if (auto* assetDb = context.services.TryGet<AssetDatabase>())
		{
			assetDb->SetModelMeshResolver(
			        [this](const std::string& path, int primitiveIndex) -> const Mesh*
			        {
				        return m_sceneCtx.assets != nullptr ? scene::ResolveModelPrimitiveMesh(*m_sceneCtx.assets, m_sceneCtx, path, primitiveIndex) : nullptr;
			        });
		}

		// The default primitive material is built lazily and acquired through the
		// MaterialRegistry per entity - nothing to register here.

		// Boot-from-scene: world content lives in the scene file and behavior in
		// entity scripts (ScriptComponent).
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
			// Loud on purpose: a game that boots into an empty world with no
			// explanation cost hours to diagnose (the "GameRuntime startup hang"
			// was exactly this - startupScene unset in the runtime's cascade).
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

		// Effects no longer own GPU pipelines (PipelineCache does, torn down in
		// AssetSubsystem::Shutdown), so there is nothing to destroy here.

		m_sceneCtx.defaultMaterialInitialized = false;
		m_sceneCtx.defaultMaterial = {};
		m_sceneCtx.meshCache.clear();
	}

	void ScriptedSceneLayer::OnUpdate(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// F5 hot-reload: reload the managed assembly and re-apply the scene.
		if (m_csharp != nullptr && m_csharp->HasReloadRequest())
		{
			m_csharp->ClearReloadRequest();
			DoReload(context);
		}
	}
} // namespace aether::app
