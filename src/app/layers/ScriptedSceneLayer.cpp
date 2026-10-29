#include "ScriptedSceneLayer.hpp"

#include <algorithm>
#include <string>

#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/RustScriptingSubsystem.hpp"
#include "utils/AetherExceptions.hpp"

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
#include "mesh/GrassMeshData.hpp"
#include "scene/Components.hpp"
#include "scene/GrassComponent.hpp"
#include "scene/GrassRuntime.hpp"
#include "material/MaterialSystem.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/PipelineCache.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "rendering/RenderTargetService.hpp"
#include "scene/ModelBakeHook.hpp"
#include <cmath>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
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
#	include "project/ProjectStartupScene.hpp"
#endif

namespace aether::app
{

	void ScriptedSceneLayer::DestroySceneEntities(LayerContext& context)
	{
		auto& world = context.Get<World>();
		m_sceneCtx.engineRuntime->RunExclusive(QuiesceMode::Drain, [&]
		{
			context.Get<AetherCore>().FlushGrassStreaming();
			for (const aether::Entity e: m_sceneCtx.sceneEntities)
			{
				world.Destroy(e);
			}
			m_sceneCtx.sceneEntities.clear();
			auto& assets = context.Get<AssetManager>();
			for (auto& model: m_sceneCtx.loadedModels)
			{
				assets.ReleaseModelTextures(model);
			}
			for (const auto& [id, root]: m_sceneCtx.grassRoots)
			{
				if (!root.material || !root.material->ready.load(std::memory_order_acquire)) continue;
				for (const auto handle: root.material->handles)
				{
					assets.GetMaterialRegistry().Release(handle);
				}
			}
			m_sceneCtx.grassRoots.clear();
			m_sceneCtx.grassMeshes.clear();
			m_sceneCtx.loadedModels.clear();
			m_sceneCtx.loadedModelMap.clear();
			m_sceneCtx.loadedModelStamps.clear();
			m_sceneCtx.meshCache.clear();
			assets.GetMaterialAuthoring().ReleaseAll();
			context.Get<Renderer>().ClearPointLights();
			context.Get<Renderer>().ClearSpotLights();
		});
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

		if (auto* rust = context.TryGet<scripting::RustScriptingSubsystem>())
		{
			if (const auto initialized = rust->Initialize(m_sceneCtx); !initialized)
			{
				AE_ERROR(LogCategory::App, "Rust gameplay initialization failed: {}", initialized.error().message);
#ifndef AETHERCORE_EDITOR_APP
				throw aether::EngineError(initialized.error().message);
#endif
			}
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
		std::string sceneName = settingsService != nullptr ? settingsService->Get().app.startupScene : std::string{};
#ifdef AETHERCORE_EDITOR_APP
		if (project != nullptr && project->IsLoaded()) sceneName = ReadProjectStartupScene(project->projectFile);
#endif
		if (sceneName.empty())
		{
			AE_WARN(LogCategory::App, "No startup scene configured (app.startupScene is empty) - booting an EMPTY world. Set it in ProjectSettings.toml / EngineSettings.toml.");
			return;
		}
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
		else if (m_sceneCtx.assets != nullptr && !sceneName.contains("://"))
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
		auto& world = context.Get<World>();
		auto& assets = context.Get<AssetManager>();
		auto staleGrass = world.GetRegistry().view<GrassRuntimeComponent>(entt::exclude<GrassComponent>);
		for (const auto entity: staleGrass)
		{
			world.GetRegistry().remove<GrassRuntimeComponent>(entity);
		}
		auto grassView = world.GetRegistry().view<GrassComponent>();
		GrassStreamingCommands streaming;
		auto& registry = assets.GetMaterialRegistry();
		const auto* camera = m_sceneCtx.cameras->TryGetMainCamera();
		auto& cameraPositions = m_sceneCtx.grassCameras;
		cameraPositions.clear();
		cameraPositions.push_back(camera ? camera->GetPosition() : glm::vec3(0.0f));
		if (auto* targets = context.TryGet<RenderTargetService>()) targets->AppendCameraPositions(cameraPositions);
		if (auto* rendering = context.TryGet<RenderingSubsystem>())
		{
			glm::vec3 preview;
			if (rendering->GetCameraPreview().TryGetCameraPosition(preview)) cameraPositions.push_back(preview);
		}
		for (auto& [key, field]: m_sceneCtx.grassMeshes)
		{
			field.desired.clear();
			field.pending.clear();
			field.active = false;
		}
		std::set<std::uint32_t> liveRoots;
		for (const auto id: grassView)
		{
			const Entity entity = World::FromEntt(id);
			const auto& grass = grassView.get<GrassComponent>(id);
			if (!world.Has<TransformComponent>(entity))
			{
				world.Emplace<TransformComponent>(entity);
			}
			if (!world.Has<MeshRendererComponent>(entity))
			{
				world.Emplace<MeshRendererComponent>(entity, MeshRendererComponent{.castShadows = false});
			}
			auto& runtime = world.GetRegistry().get_or_emplace<GrassRuntimeComponent>(id);
			runtime.draws.clear();
			runtime.selected.clear();
			if (!grass.enabled)
			{
				continue;
			}
			auto key = MeshGen::GrassMeshKey(grass);
			runtime.key = key;
			auto fieldIt = m_sceneCtx.grassMeshes.find(key);
			if (fieldIt == m_sceneCtx.grassMeshes.end())
			{
				if (const auto* hook = context.TryGet<scene::ModelBakeHook>(); hook && hook->ensureBaked)
				{
					for (const auto* path: {&grass.surfaceMesh, &grass.exclusionMesh})
					{
						if (path->empty()) continue;
						std::string error;
						if (!hook->ensureBaked(*path, error))
						{
							AE_WARN(LogCategory::App, "Grass source '{}' import failed: {}", *path, error);
						}
					}
					key = MeshGen::GrassMeshKey(grass);
					runtime.key = key;
					fieldIt = m_sceneCtx.grassMeshes.find(key);
				}
				if (fieldIt == m_sceneCtx.grassMeshes.end())
				{
					auto prepared = MeshGen::GrassField::Load(grass);
					if (!prepared)
					{
						AE_WARN(LogCategory::App, "Grass generation failed: {}", prepared.error());
						continue;
					}
					fieldIt = m_sceneCtx.grassMeshes.try_emplace(key).first;
					fieldIt->second.field = std::move(*prepared);
				}
			}
			auto& field = fieldIt->second;
			field.active = true;
			const glm::mat4 inverse = glm::inverse(world.Get<TransformComponent>(entity).localToWorld);
			const glm::mat3 basis(inverse);
			const glm::mat3 gram = glm::transpose(basis) * basis;
			float maxRow = 0.0f;
			for (int row = 0; row < 3; ++row)
			{
				maxRow = std::max(maxRow, std::abs(gram[0][row]) + std::abs(gram[1][row]) + std::abs(gram[2][row]));
			}
			const float scale = std::sqrt(maxRow);
			for (const glm::vec3 cameraPosition: cameraPositions)
			{
				const glm::vec3 localCamera = glm::vec3(inverse * glm::vec4(cameraPosition, 1.0f));
				auto& selected = m_sceneCtx.grassSelectionScratch;
				if (auto result = field.field.SelectChunks({localCamera.x, localCamera.z}, selected, scale); !result)
				{
					AE_WARN(LogCategory::App, "Grass chunk selection failed: {}", result.error());
					continue;
				}
				runtime.selected.insert(runtime.selected.end(), selected.begin(), selected.end());
				field.pending.insert(field.pending.end(), selected.begin(), selected.end());
				field.desired.insert(field.desired.end(), selected.begin(), selected.end());
			}
			std::ranges::sort(runtime.selected);
			runtime.selected.erase(std::unique(runtime.selected.begin(), runtime.selected.end()), runtime.selected.end());
			const auto rootId = static_cast<std::uint32_t>(id);
			liveRoots.insert(rootId);
			auto& root = m_sceneCtx.grassRoots[rootId];
			const auto* authored = world.TryGet<MaterialComponent>(entity);
			const auto source = authored ? authored->handle : MaterialHandle{};
			std::array<glm::vec2, 3> fades;
			for (std::uint32_t lod = 0; lod < 3; ++lod) fades[lod] = field.field.DistanceFade(lod);
			if (!(root.source == source) || root.windStrength != grass.windStrength || root.fades != fades || !root.material)
			{
				MaterialAsset material;
				const bool held = source.IsValid() && registry.AddRef(source);
				if (held) registry.TryDescribe(source, material);
				material.grass = true;
				material.grassWindStrength = grass.windStrength;
				material.doubleSided = true;
				material.modulateVertexColor = true;
				material.alphaMask = false;
				material.alphaBlend = false;
				material.additiveBlend = false;
				material.foliage = true;
				if (root.material) streaming.retiredMaterials.push_back(std::move(root.material));
				root.material = std::make_shared<GrassMaterialState>();
				streaming.materials.push_back({root.material, std::move(material), held ? source : MaterialHandle{}, fades});
				root.source = source;
				root.windStrength = grass.windStrength;
				root.fades = fades;
			}
			runtime.pipeline = root.material->ready.load(std::memory_order_acquire) ? root.material->pipeline : nullptr;
		}
		for (auto& [key, field]: m_sceneCtx.grassMeshes)
		{
			std::ranges::sort(field.desired);
			field.desired.erase(std::unique(field.desired.begin(), field.desired.end()), field.desired.end());
		}
		std::erase_if(m_sceneCtx.grassRoots, [&](const auto& entry)
		{
			if (liveRoots.contains(entry.first)) return false;
			if (entry.second.material) streaming.retiredMaterials.push_back(entry.second.material);
			return true;
		});
		for (auto& [key, field]: m_sceneCtx.grassMeshes)
		{
			std::erase_if(field.chunks, [&](const auto& chunk)
			{
				if (std::binary_search(field.desired.begin(), field.desired.end(), chunk.first) && !(chunk.second->ready.load(std::memory_order_acquire) && chunk.second->failed)) return false;
				streaming.retired.push_back(chunk.second);
				return true;
			});
		}
		std::erase_if(m_sceneCtx.grassMeshes, [](const auto& entry) { return !entry.second.active; });
		for (auto& [key, field]: m_sceneCtx.grassMeshes)
		{
			for (const auto chunkKey: field.pending)
			{
				if (streaming.uploads.size() == 8) break;
				if (field.chunks.contains(chunkKey)) continue;
				auto generated = field.field.GenerateChunk(chunkKey);
				if (!generated)
				{
					AE_WARN(LogCategory::App, "Grass chunk generation failed: {}", generated.error());
					continue;
				}
				auto chunk = std::make_shared<GrassRuntimeChunk>();
				chunk->key = chunkKey;
				chunk->origin = generated->origin;
				field.chunks.emplace(chunkKey, chunk);
				streaming.uploads.push_back({std::move(chunk), std::move(generated->mesh)});
			}
		}
		if (!streaming.uploads.empty() || !streaming.materials.empty() || !streaming.retired.empty() || !streaming.retiredMaterials.empty())
		{
			context.Get<AetherCore>().QueueGrassStreaming(std::move(streaming));
		}
		for (const auto id: grassView)
		{
			auto* runtime = world.GetRegistry().try_get<GrassRuntimeComponent>(id);
			if (runtime == nullptr) continue;
			const auto field = m_sceneCtx.grassMeshes.find(runtime->key);
			const auto root = m_sceneCtx.grassRoots.find(static_cast<std::uint32_t>(id));
			if (field == m_sceneCtx.grassMeshes.end() || root == m_sceneCtx.grassRoots.end()) continue;
			if (!root->second.material || !root->second.material->ready.load(std::memory_order_acquire)) continue;
			for (const auto chunkKey: runtime->selected)
			{
				const auto chunk = field->second.chunks.find(chunkKey);
				if (chunk != field->second.chunks.end() && chunk->second->ready.load(std::memory_order_acquire) && chunk->second->mesh.IsValid() && chunk->second->mesh.IsIndexed())
				{
					runtime->draws.push_back({chunk->second.get(), root->second.material->slots[chunkKey.lod]});
				}
			}
		}

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
