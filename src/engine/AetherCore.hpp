#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>

#include "BindlessManager.hpp"
#include "Camera.hpp"
#include "CameraManager.hpp"
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
#include "UniqueImage.hpp"
#include "VulkanContext.hpp"
#include "Window.hpp"
#include "World.hpp"

namespace aether
{
	class AetherCore
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			int width = 1280;
			int height = 720;
		};

		// Opaque handle to a render-to-texture camera target.
		struct CameraRenderTarget
		{
			uint32_t id = 0;
			[[nodiscard]] bool IsValid() const { return id != 0; }
		};

		explicit AetherCore(const Config& config = {});
		~AetherCore();

		[[nodiscard]] bool ShouldClose() const;
		void PumpEvents() const;
		// Must be called once per frame BEFORE layer OnUpdate().
		// Updates input state and advances all non-Manual cameras.
		void Tick(float dt);
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

		// ── Camera system ─────────────────────────────────────────────────────
		[[nodiscard]] CameraManager& GetCameraManager();
		[[nodiscard]] const CameraManager& GetCameraManager() const;

		[[nodiscard]] CameraRenderTarget CreateCameraRenderTarget(CameraHandle camera, VkExtent2D extent);
		void DestroyCameraRenderTarget(CameraRenderTarget rt);
		
		[[nodiscard]] RGImage GetRenderTargetColorImage(CameraRenderTarget rt) const;
		[[nodiscard]] uint32_t GetRenderTargetBindlessSlot(CameraRenderTarget rt) const;

	private:
		void RecreateSwapchain();
		void RegisterPasses();
		void RegisterRttPassesFor(uint32_t id);
		void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

		// Per-camera render-to-texture entry.
		struct CameraRtEntry
		{
			CameraHandle                          camera;
			VkExtent2D                            extent;
			UniqueImage                           colorImage;
			UniqueImage                           depthImage;
			RGImage                               rgColor{};
			RGImage                               rgDepth{};
			std::unique_ptr<FrameConstantsBuffer> constants;
		};

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
		CameraManager m_cameraManager;

		std::unordered_map<uint32_t, CameraRtEntry> m_rtCameras;
		uint32_t m_nextRtId = 1;
	};
}