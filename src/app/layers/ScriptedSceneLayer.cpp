#include "ScriptedSceneLayer.hpp"

#include <filesystem>

#include "scripting/ScriptingSubsystem.hpp"
#include "scripting/SystemFactory.hpp"

#include "assets/AssetManager.hpp"
#include "assets/AssetSubsystem.hpp"
#include "camera/CameraManager.hpp"
#include "effects/EffectManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "layers/LoadingLayer.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "passes/PostProcessStack.hpp"
#include "platform/Input.hpp"
#include "rendering/RenderThread.hpp"
#include "rendering/LightingManager.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/LocalShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "gpu/GpuDevice.hpp"
#include "scene/World.hpp"
#include "physics/PhysicsSystem.hpp"
#include "animation/AnimationIk.hpp"
#include "systems/DayNightSystem.hpp"
#include "utils/Logger.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/GpuEnumConversions.hpp"

namespace aether::app::scripting
{
	void InitPhysicsModule(aether::PhysicsSystem* physics);
	void InitAnimationModule(aether::AnimationIkSystem* ik);
	void SetPhysicsDebugRendererCallback(std::function<void(bool)> callback);
} // namespace aether::app::scripting

namespace aether::app
{
	ScriptedSceneLayer::ScriptedSceneLayer(std::string scriptPath)
	      : m_scriptPath(std::move(scriptPath))
	{
	}

	// -- Helpers ---------------------------------------------------------------

	void ScriptedSceneLayer::BuildDefaultPipeline(LayerContext& context)
	{
		auto& assets = context.Get<AssetManager>();

		auto result = assets.CreateGraphicsPipeline({
		        .shaderVfsPath = "shaders://gltf_mesh.spv",
		        .colorFormat = aether::PostProcessStack::GetForwardColorFormat(),
		        .depthFormat = context.Get<Swapchain>().GetDepthFormat(),
		        .depthTestEnable = true,
		        .depthWriteEnable = true,
		        .descriptorHeapMappings = context.Get<BindlessManager>().GetDescriptorHeapMappings(),
		});

		if (!result)
		{
			AE_ERROR(LogCategory::App, "ScriptedSceneLayer: failed to create default pipeline");
			return;
		}
		m_defaultPipeline = std::move(result.value());
	}

	void ScriptedSceneLayer::DestroySceneEntities(LayerContext& context)
	{
		auto& world = context.Get<World>();
		for (aether::Entity e: m_sceneCtx.sceneEntities)
		{
			world.Destroy(e);
		}
		m_sceneCtx.sceneEntities.clear();
		m_sceneCtx.loadedModels.clear();
		m_sceneCtx.loadedModelMap.clear();
		m_sceneCtx.meshCache.clear();

		for (const auto& name: m_sceneCtx.registeredSystems)
		{
			world.UnregisterSystem(name.c_str());
		}
		m_sceneCtx.registeredSystems.clear();

		// Clear script-created lights so reload doesn't stack duplicates.
		context.Get<Renderer>().ClearPointLights();
		context.Get<Renderer>().ClearSpotLights();
	}

	void ScriptedSceneLayer::DoReload(LayerContext& context)
	{
		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reloading '{}'", m_scriptPath);

		if (m_handle.IsValid())
		{
			m_scripting->CallOnDetach(m_handle, m_sceneCtx);

			// Signal that a reload is in progress - the render thread will skip
			// executing any new frames until we've destroyed the old scene entities.
			context.Get<aether::RenderThread>().SetReloadInProgress(true);

			// Frames in flight may still reference the buffers backing the scene
			// entities we're about to destroy. Hot-reload is out-of-band, so a
			// device-wide wait is acceptable.
			context.Get<GpuDevice>().WaitIdle();

			// Clear all render queues to remove any pending commands that reference
			// destroyed meshes/animation databases.
			auto& renderQueue = context.Get<RenderQueue>();
			for (std::uint32_t i = 0; i < RenderQueue::kFramesInFlight; ++i)
			{
				renderQueue.Clear(i);
			}
			if (auto shadowService = context.TryGet<ShadowService>())
			{
				shadowService->ClearAllQueues();
			}
			if (auto localShadowService = context.TryGet<LocalShadowService>())
			{
				localShadowService->ClearAllQueues();
			}

			DestroySceneEntities(context);

			context.Get<aether::RenderThread>().SetReloadInProgress(false);
		}

		scripting::ScriptHandle newHandle = m_scripting->Compile(m_scriptPath);
		if (!newHandle.IsValid())
		{
			m_scriptBroken = true;
			AE_WARN(LogCategory::App, "ScriptedSceneLayer: reload failed");
			if (m_handle.IsValid())
			{
				AE_WARN(LogCategory::App, "Keeping old scene");
				m_scripting->CallOnAttach(m_handle, m_sceneCtx);
			}
			return;
		}

		// Swap to the new script and call on_attach.
		if (m_handle.IsValid())
		{
			m_scripting->FreeHandle(m_handle);
		}
		m_handle = std::move(newHandle);
		m_scriptBroken = false;
		m_scripting->ClearErrors();
		m_scripting->CallOnAttach(m_handle, m_sceneCtx);

		AE_INFO(LogCategory::App, "ScriptedSceneLayer: reload complete.");
	}

	// -- AppLayer overrides ----------------------------------------------------

	void ScriptedSceneLayer::OnAttach(LayerContext& context)
	{
		m_scripting = context.TryGet<scripting::ScriptingSubsystem>();
		if (!m_scripting)
		{
			AE_ERROR(LogCategory::App, "ScriptedSceneLayer: ScriptingSubsystem not in ServiceContainer");
			return;
		}

		BuildDefaultPipeline(context);

		// Register effects so script can use set_entity_effect().
		{
			const auto colorFormat = aether::PostProcessStack::GetForwardColorFormat();
			const auto depthFormat = context.Get<Swapchain>().GetDepthFormat();

			aether::Material plasmaMat{};
			plasmaMat.baseColorFactor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
			plasmaMat.emissiveFactor = glm::vec3(1.0f, 0.3f, 0.8f); // tint = pink
			plasmaMat.metallicFactor = 0.5f;                        // speed
			plasmaMat.roughnessFactor = 2.0f;                       // scale
			plasmaMat.occlusionStrength = 0.8f;                     // intensity

			m_effectManager.CreateAndRegister("plasma", context.Get<AssetManager>(), context.Get<BindlessManager>().GetDescriptorHeapMappings(), colorFormat, depthFormat, "shaders://plasma.spv", plasmaMat);
		}

		m_sceneCtx.world = &context.Get<World>();
		m_sceneCtx.assets = &context.Get<AssetManager>();
		m_sceneCtx.cameras = &context.Get<CameraManager>();
		m_sceneCtx.renderer = &context.Get<Renderer>();
		m_sceneCtx.input = &context.Get<Input>();
		m_sceneCtx.effects = &m_effectManager;
		m_sceneCtx.loadingOverlay = context.TryGet<aether::app::LoadingLayer>();
		if (auto assetsSub = context.TryGet<aether::AssetSubsystem>())
		{
			m_sceneCtx.uploadPool = assetsSub->GetUploadContext().GetCommandPool();
		}
		if (auto dn = context.TryGet<aether::app::DayNightSystem>())
		{
			m_sceneCtx.dayNight = dn;
		}
		m_sceneCtx.systemFactory = context.TryGet<aether::app::SystemFactory>();
		m_sceneCtx.defaultPipeline = &m_defaultPipeline;
		m_sceneCtx.primitives = &context.Get<PrimitiveMeshes>();
		if (auto physSys = context.Get<World>().FindSystem("PhysicsSystem"))
		{
			m_sceneCtx.physics = static_cast<aether::PhysicsSystem*>(physSys);
			aether::app::scripting::InitPhysicsModule(m_sceneCtx.physics);
		}
		if (auto ikSys = context.TryGet<aether::AnimationIkSystem>())
		{
			aether::app::scripting::InitAnimationModule(ikSys);
		}
		m_sceneCtx.scriptPath = m_scriptPath;

		// Register a default white material for primitive meshes.
		if (!m_sceneCtx.defaultMaterialRegistered)
		{
			m_sceneCtx.defaultMaterial = {};
			m_sceneCtx.defaultMaterial.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.f);
			m_sceneCtx.defaultMaterial.roughnessFactor = 0.6f;
			m_sceneCtx.defaultMaterial.metallicFactor = 0.0f;
			context.Get<AssetManager>().RegisterMaterial(m_sceneCtx.defaultMaterial);
			m_sceneCtx.defaultMaterialRegistered = true;
		}

		m_handle = m_scripting->Compile(m_scriptPath);
		if (!m_handle.IsValid())
		{
			m_scriptBroken = true;
			AE_WARN(LogCategory::App, "ScriptedSceneLayer: initial compile failed for '{}'", m_scriptPath);
			return;
		}

		m_scriptBroken = false;
		m_scripting->ClearErrors();
		m_scripting->CallOnAttach(m_handle, m_sceneCtx);
	}

	void ScriptedSceneLayer::OnDetach(LayerContext& context)
	{
		if (!m_scripting)
		{
			return;
		}
		m_scripting->CallOnDetach(m_handle, m_sceneCtx);
		DestroySceneEntities(context);

		m_effectManager.DestroyAll(context.Get<AssetManager>());

		if (m_sceneCtx.defaultMaterialRegistered)
		{
			context.Get<AssetManager>().UnregisterMaterial(m_sceneCtx.defaultMaterial);
			m_sceneCtx.defaultMaterialRegistered = false;
			m_sceneCtx.defaultMaterial = {};
		}
		m_sceneCtx.meshCache.clear();

		m_scripting->FreeHandle(m_handle);
	}

	void ScriptedSceneLayer::OnUpdate(LayerContext& context)
	{
		if (!m_scripting)
		{
			return;
		}

		// Handle hot-reload even when the script hasn't compiled yet
		// (e.g. initial compile failed) so the user can fix errors via F5.
		if (m_scripting->HasReloadRequest())
		{
			m_scripting->ClearReloadRequest();
			DoReload(context);
			return;
		}

		if (!m_handle.IsValid())
		{
			return;
		}

		m_sceneCtx.deltaTime = static_cast<float>(context.deltaTimeSeconds);

		m_scripting->CallOnUpdate(m_handle, m_sceneCtx);
	}
} // namespace aether::app
