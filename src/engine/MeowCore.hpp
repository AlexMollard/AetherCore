#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "BindlessManager.hpp"
#include "CommandRecorder.hpp"
#include "FrameConstantsBuffer.hpp"
#include "GraphicsPipeline.hpp"
#include "Input.hpp"
#include "Mesh.hpp"
#include "PostProcessStack.hpp"
#include "PrimitiveMeshes.hpp"
#include "RenderGraph.hpp"
#include "RenderQueue.hpp"
#include "ResourcePool.hpp"
#include "Scene.hpp"
#include "Swapchain.hpp"
#include "Texture.hpp"
#include "VulkanContext.hpp"
#include "Window.hpp"
#include "World.hpp"

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
		[[nodiscard]] RenderGraph& GetRenderGraph();
		[[nodiscard]] const RenderGraph& GetRenderGraph() const;
		[[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const;
		[[nodiscard]] VkFormat   GetSwapchainImageFormat() const;
		[[nodiscard]] VkFormat   GetSwapchainDepthFormat() const;
		// Format of the color attachment used by the engine's forward pass.
		// All game-layer pipelines that render scene geometry must use this format.
		[[nodiscard]] static constexpr VkFormat GetForwardColorFormat() { return PostProcessStack::GetForwardColorFormat(); }
		[[nodiscard]] VkExtent2D GetSwapchainExtent() const;
		[[nodiscard]] RenderQueue* GetRenderQueue();
		[[nodiscard]] Scene* GetScene();
		[[nodiscard]] World& GetWorld();
		[[nodiscard]] const World& GetWorld() const;
		[[nodiscard]] const Mesh& GetPrimitiveMesh(PrimitiveMesh primitive) const;
		[[nodiscard]] GraphicsPipeline CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);
		[[nodiscard]] Mesh             CreateMesh(std::span<const Mesh::Vertex> vertices);
		[[nodiscard]] Texture          CreateTexture(std::string_view path);

		void SetTonemapMode(TonemapMode mode);
		[[nodiscard]] TonemapMode GetTonemapMode() const;
		void SetFxaaEnabled(bool enabled);
		[[nodiscard]] bool IsFxaaEnabled() const;

		[[nodiscard]] Input& GetInput();
		[[nodiscard]] const Input& GetInput() const;

	private:
		void RecreateSwapchain();
		void RegisterPasses();
		void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

		Window m_window;
		VulkanContext m_vulkanContext;
		Swapchain m_swapchain;
		BindlessManager m_bindlessManager;
		FrameConstantsBuffer m_frameConstantsBuffer;
		ResourcePool m_resourcePool;
		RenderGraph m_renderGraph;
		RenderQueue m_renderQueue;
		Scene m_scene;
		World m_world;
		PrimitiveMeshes m_primitiveMeshes;
		CommandRecorder m_currentRecorder; // engine-internal, filled by BeginFrame
		std::uint64_t m_frameIndex = 0;
		VkCommandPool m_uploadPool = VK_NULL_HANDLE;

		// Manages all offscreen targets and post-processing pipelines.
		// Recreated on swapchain resize.
		PostProcessStack m_postProcessStack;
		Input m_input;
	};
}