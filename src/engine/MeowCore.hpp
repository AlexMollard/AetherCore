#pragma once

#include <cstdint>
#include <span>

#include "BindlessManager.hpp"
#include "CommandRecorder.hpp"
#include "GraphicsPipeline.hpp"
#include "Mesh.hpp"
#include "PrimitiveMeshes.hpp"
#include "RenderQueue.hpp"
#include "ResourcePool.hpp"
#include "Scene.hpp"
#include "Swapchain.hpp"
#include "VulkanContext.hpp"
#include "Window.hpp"

namespace meow
{
	class MeowCore
	{
	public:
		struct Config
		{
			const char* appName = "MeowCore";
			int width = 1280;
			int height = 720;
		};

		explicit MeowCore(const Config& config = {});
		~MeowCore();

		[[nodiscard]] bool ShouldClose() const;
		void PumpEvents() const;
		void BeginFrame();
		void EndFrame();
		void WaitIdle() const;

		[[nodiscard]] Window& GetWindow();
		[[nodiscard]] const Window& GetWindow() const;
		[[nodiscard]] VulkanContext& GetVulkanContext();
		[[nodiscard]] const VulkanContext& GetVulkanContext() const;
		[[nodiscard]] BindlessManager& GetBindlessManager();
		[[nodiscard]] const BindlessManager& GetBindlessManager() const;
		[[nodiscard]] ResourcePool& GetResourcePool();
		[[nodiscard]] const ResourcePool& GetResourcePool() const;
		[[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const;
		[[nodiscard]] VkFormat GetSwapchainImageFormat() const;
		[[nodiscard]] RenderQueue* GetRenderQueue();
		[[nodiscard]] Scene* GetScene();
		[[nodiscard]] const Mesh& GetPrimitiveMesh(PrimitiveMesh primitive) const;
		[[nodiscard]] GraphicsPipeline CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);
		[[nodiscard]] Mesh             CreateMesh(std::span<const Mesh::Vertex> vertices);

	private:
		Window m_window;
		VulkanContext m_vulkanContext;
		Swapchain m_swapchain;
		BindlessManager m_bindlessManager;
		ResourcePool m_resourcePool;
		RenderQueue m_renderQueue;
		Scene m_scene;
		PrimitiveMeshes m_primitiveMeshes;
		CommandRecorder m_currentRecorder; // engine-internal, filled by BeginFrame
		std::uint64_t m_frameIndex = 0;
	};
}