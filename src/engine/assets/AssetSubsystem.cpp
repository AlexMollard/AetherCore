#include "assets/AssetSubsystem.hpp"

#include <stdexcept>

#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "material/EffectSystem.hpp"
#include "material/Texture.hpp"
#include "ui/UiImageSystem.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "scene/World.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/DiagnosticEngine.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "gpu/OneShotCmd.hpp"

namespace aether
{
	void AssetSubsystem::Init(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		services.Register<PrimitiveMeshes>(m_primitiveMeshes);

		m_context = &services.Get<VulkanContext>();
		VulkanContext& vk = *m_context;
		auto& bindless = services.Get<BindlessManager>();
		auto& world = services.Get<World>();

		// Create a one-shot upload context backed by the resource registry
		auto& registry = services.Get<aether::ResourceRegistry>();
		m_uploadContext = gpu::UploadContext::Create(static_cast<void*>(vk.GetDevice().device), vk.GetGraphicsQueueFamily(), static_cast<void*>(vk.GetGraphicsQueue()), static_cast<void*>(&registry));

		m_materialBuffer.Initialize();
		m_effectParamBuffer.Initialize();
		m_textureSink.Initialize(vk, m_uploadContext, bindless.GetCapacity());
		// STALE texture handles (a texture freed out from under a live material)
		// resolve to this 1x1 magenta marker, so they show as a visible error
		// rather than silently sampling base colour. A material with NO texture
		// authored still packs kNoTexture and skips the sample - magenta is only
		// for the stale/missing case. Synthesized in-binary (not an asset) so the
		// error texture can never itself fail to load.
		AE_EXPECT_OR_THROW(magentaFallback, Texture::CreateSolidColor({255, 0, 255, 255}, vk.GetDevice().device, vk.GetGraphicsQueue(), m_uploadContext.GetCommandPool()));
		m_textureRegistry.InitializeDefault(TextureResource{std::move(magentaFallback)});

		MaterialAsset defaultAsset;
		defaultAsset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
		defaultAsset.roughnessFactor = 0.6f;
		defaultAsset.metallicFactor = 0.0f;
		m_materialRegistry.InitializeDefault(defaultAsset);

		m_meshArena.Initialize(vk, {});

		// Wire GPU memory tracking to the arena's GpuHeap instances so
		// crash-diagnostic address resolution can identify vertex/index
		// heap ranges by name.
		if (services.TryGet<DiagnosticEngine>() != nullptr)
		{
			m_meshArena.SetMemoryTracker(&services.Get<DiagnosticEngine>().GetMemoryTracker());
		}

		m_meshUploadQueue.Initialize();
		m_primitiveMeshes.Initialize(m_uploadContext);
		// The asset catalog is now resolvable for built-in primitives; the app layer
		// injects the glTF model-mesh resolver once the SceneContext model cache
		// exists (ScriptedSceneLayer::OnAttach).
		services.Register<AssetDatabase>(m_assetDatabase);
		m_assetDatabase.RegisterBuiltinPrimitives();
		m_world = &world;
		m_assetManager.Initialize(vk, bindless, m_materialRegistry, m_materialAuthoring, m_pipelineCache, m_effectParamBuffer, m_textureRegistry, world, m_uploadContext);
		EffectSystem::ConnectLifecycle(world, m_effectParamBuffer);
		// UIImage.texture is Acquire()'d by the scene loader (SceneSerializer::ApplyScene);
		// release it here on destroy/ReplaceScene's destroy-all, mirroring Material/Effect.
		UiImageSystem::ConnectLifecycle(world, m_textureRegistry);
	}

	void AssetSubsystem::InitializePipelineCache(PipelineCache::Context context)
	{
		m_pipelineCache.Initialize(context, [this](const GraphicsPipeline::Desc& desc) { return m_assetManager.CreateGraphicsPipeline(desc); });
	}

	void AssetSubsystem::LinkRenderingDeps(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		m_assetManager.SetRenderQueue(services.Get<RenderQueue>());
		m_assetManager.SetShadowService(services.Get<ShadowService>());
		m_assetManager.SetRenderTargetService(services.Get<RenderTargetService>());
	}

	void AssetSubsystem::AdvanceFrame(std::uint64_t frameIndex)
	{
		m_materialBuffer.AdvanceFrame(frameIndex);
		m_effectParamBuffer.AdvanceFrame(frameIndex);
	}

	void AssetSubsystem::FlushMeshUploads()
	{
		AE_PROFILE_ZONE();
		if (!m_meshUploadQueue.HasPendingUploads())
		{
			return;
		}

		VulkanContext& vk = *m_context;
		void* device = static_cast<void*>(vk.GetDevice().device);
		void* pool = m_uploadContext.GetCommandPool();
		void* queue = static_cast<void*>(vk.GetGraphicsQueue());

		gpu::OneShotCmd cmd;
		if (!cmd.Begin(device, pool))
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to begin one-shot command buffer."));
		}
		m_meshUploadQueue.Flush(cmd.CmdList());
		if (!cmd.EndAndSubmit(queue))
		{
			Throw(AetherError::Vulkan(0, "AssetSubsystem: failed to submit mesh upload."));
		}
	}

	void AssetSubsystem::Shutdown()
	{
		AE_PROFILE_ZONE();
		// The world may outlive this subsystem; make sure late component teardown
		// cannot release handles into a destroyed registry.
		if (m_world != nullptr)
		{
			MaterialSystem::DisconnectLifecycle(*m_world);
			EffectSystem::DisconnectLifecycle(*m_world);
			UiImageSystem::DisconnectLifecycle(*m_world);
			m_world = nullptr;
		}
		// Pure-CPU bookkeeping drop (no registry/sink calls); safe before or after
		// the buffer shuts down.
		m_materialAuthoring.ReleaseAll();
		// Destroy all texture entries while the BindlessManager is still alive so
		// their sampled-image slots retire on the existing deferred-free clock.
		m_textureRegistry.ReleaseAll();
		m_assetManager = AssetManager{};
		m_primitiveMeshes.Destroy();
		m_meshUploadQueue.Shutdown();
		m_meshArena.Shutdown();
		m_materialBuffer.Shutdown();
		m_effectParamBuffer.Shutdown();
		// The cache borrows BindlessManager's heap mappings; it must shut down
		// before BindlessManager. AssetSubsystem tears down before GpuDevice, and
		// this runs inside the engine's GPU-idle window, so deferred pipeline
		// destruction is safe.
		m_pipelineCache.Shutdown();

		if (m_uploadContext.IsValid())
		{
			m_uploadContext.Destroy();
		}
	}
} // namespace aether
