#include "ScriptedSceneLayer.hpp"

#include "scripting/CSharpScriptingSubsystem.hpp"

#include "IEngineRuntime.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetSubsystem.hpp"
#include "material/MaterialAuthoring.hpp"
#include "camera/CameraManager.hpp"
#include "material/EffectManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Input.hpp"
#include "scene/SceneSerializer.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "gpu/GpuDevice.hpp"
#include "scene/World.hpp"
#include "physics/PhysicsSystem.hpp"
#include "systems/DayNightSystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "vulkan/Swapchain.hpp"
#include "utils/Profiler.hpp"

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
		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reloading scene + entity scripts");

		// Hot-reload frees the buffers backing the scene entities. Route the
		// teardown through the engine's exclusive-mutation primitive in Discard
		// mode: the render thread drops in-flight frames (which still reference
		// those buffers) as it parks, rather than draining and rendering them
		// against freed memory. RunExclusive supplies the park + GPU WaitIdle.
		// This runs regardless of the main script: the scene file loaded
		// entities that reference the model cache DestroySceneEntities frees.
		context.Get<aether::IEngineRuntime>().RunExclusive(aether::QuiesceMode::Discard,
		        [&]()
		        {
			        // Clear render queues that reference destroyed meshes /
			        // animation databases BEFORE destroying the entities.
			        context.Get<RenderQueue>().DiscardAllPending();
			        if (auto shadowService = context.TryGet<ShadowService>())
			        {
				        shadowService->ClearAllQueues();
			        }
			        if (auto localShadowService = context.TryGet<LocalShadowService>())
			        {
				        localShadowService->ClearAllQueues();
			        }
			        DestroySceneEntities(context);
		        });

		// DestroySceneEntities above tore down the FILE-loaded world; re-load
		// it (or a broken-reload F5 would leave a void that a later save
		// captures).
		LoadStartupScene(context);

		// Tear down every live managed instance before reloading the assembly so
		// the collectible load context can unload (no live GCHandles remain).
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

		// The default primitive material is built lazily and acquired through the
		// MaterialRegistry per entity - nothing to register here.

		// Boot-from-scene: world content lives in the scene file and behavior in
		// entity scripts (ScriptComponent).
		LoadStartupScene(context);
	}

	void ScriptedSceneLayer::LoadStartupScene(LayerContext& context)
	{
		const auto* settings = context.TryGet<aether::EngineSettings>();
		if (settings == nullptr || settings->app.startupScene.empty())
		{
			return;
		}
		const std::string& sceneName = settings->app.startupScene;
		scene::ApplySceneDeps deps{};
		deps.assets = m_sceneCtx.assets;
		deps.primitives = m_sceneCtx.primitives;
		deps.effectManager = m_sceneCtx.effects;
		deps.effectParams = m_sceneCtx.assets ? &m_sceneCtx.assets->GetEffectParamBuffer() : nullptr;
		deps.pipelines = m_sceneCtx.assets ? &m_sceneCtx.assets->GetPipelineCache() : nullptr;
		deps.sceneContext = &m_sceneCtx;
		deps.physics = m_sceneCtx.physics;
		deps.renderer = m_sceneCtx.renderer;

		if (const auto desc = scene::ReadSceneFile(sceneName))
		{
			scene::ApplyScene(*desc, context.Get<World>(), deps);
			AE_INFO(LogCategory::App, "Startup scene '{}' loaded ({} entities)", sceneName, desc->entities.size());
		}
		else if (m_sceneCtx.assets != nullptr)
		{
			const auto captured = scene::CaptureScene(context.Get<World>(), m_sceneCtx.assets->GetMaterialRegistry(), m_sceneCtx.assets->GetTextureRegistry(), m_sceneCtx.renderer);
			if (scene::SaveSceneFile(sceneName, captured))
			{
				AE_INFO(LogCategory::App, "Startup scene '{}' auto-generated from script content - commit resources/scenes/{}.scene.toml", sceneName, sceneName);
			}
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
