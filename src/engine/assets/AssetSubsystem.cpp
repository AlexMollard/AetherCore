#include "assets/AssetSubsystem.hpp"

#include <stdexcept>

#include "utils/Profiler.hpp"
#include "utils/ServiceContainer.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "rendering/ShadowService.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "scene/World.hpp"
#include "vulkan/ResourcePool.hpp"
#include "vulkan/VulkanContext.hpp"
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

		m_materialBuffer.Initialize(vk);
		m_meshArena.Initialize(vk, {});
		m_meshUploadQueue.Initialize(vk);
		m_primitiveMeshes.Initialize(m_uploadContext);
		m_assetManager.Initialize(vk, bindless, m_materialBuffer, world, m_uploadContext);
	}

	void AssetSubsystem::LinkRenderingDeps(ServiceContainer& services)
	{
		AE_PROFILE_ZONE();
		m_assetManager.SetRenderQueue(services.Get<RenderQueue>());
		m_assetManager.SetShadowService(services.Get<ShadowService>());
		m_assetManager.SetRenderTargetService(services.Get<RenderTargetService>());
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
		m_assetManager = AssetManager{};
		m_primitiveMeshes.Destroy();
		m_meshUploadQueue.Shutdown();
		m_meshArena.Shutdown();
		m_materialBuffer.Shutdown();

		if (m_uploadContext.IsValid())
		{
			m_uploadContext.Destroy();
		}
	}
} // namespace aether
