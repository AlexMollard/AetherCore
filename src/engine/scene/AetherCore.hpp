#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "animation/AnimationDatabase.hpp"
#include "utils/AssetManager.hpp"
#include "material/BindlessManager.hpp"
#include "camera/Camera.hpp"
#include "camera/CameraManager.hpp"
#include "rendering/CommandRecorder.hpp"
#include "passes/CullPass.hpp"
#include "utils/EngineSettings.hpp"
#include "passes/ForwardPass.hpp"
#include "passes/FrameComposer.hpp"
#include "rendering/FrameConstants.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/GraphicsPipeline.hpp"
#include "platform/Input.hpp"
#include "camera/LightingManager.hpp"
#include "material/Material.hpp"
#include "material/MaterialBuffer.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/MeshArena.hpp"
#include "mesh/MeshUploadQueue.hpp"
#include "animation/ModelAnimator.hpp"
#include "passes/PostProcessStack.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderPipelineCoordinator.hpp"
#include "rendering/RenderQueue.hpp"
#include "rendering/RenderTargetService.hpp"
#include "rendering/RenderThread.hpp"
#include "vulkan/ResourcePool.hpp"
#include "scene/Scene.hpp"
#include "passes/ShadowService.hpp"
#include "passes/SkyboxPass.hpp"
#include "vulkan/Swapchain.hpp"
#include "material/Texture.hpp"
#include "vulkan/UniqueBuffer.hpp"
#include "vulkan/UniqueImage.hpp"
#include "vulkan/VulkanContext.hpp"
#include "platform/Window.hpp"
#include "scene/World.hpp"

namespace aether
{
	struct LoadedModelPrimitive
	{
		Mesh mesh;
		Material material{};
		glm::mat4 localTransform{ 1.0f }; // Identity for skinned primitives.
		std::int32_t skinIndex = -1;      // -1 means not skinned.
	};

	struct LoadedModel
	{
		std::vector<Texture> textures;
		std::vector<LoadedModelPrimitive> primitives;
		std::optional<ModelAnimator> animator;
		AnimationDatabase animationDb; // GPU-friendly animation clip data.
	};

	class AetherCore
	{
	public:
		struct Config
		{
			const char* appName = "AetherCore";
			int width = 1280;
			int height = 720;
			bool enableVsync = true;
			const char* settingsFile = "engine.toml";
		};

		// Opaque handle to a render-to-texture camera target.
		struct CameraRenderTarget
		{
			uint32_t id = 0;

			[[nodiscard]] bool IsValid() const
			{
				return id != 0;
			}
		};

		explicit AetherCore(const Config& config = {});
		AetherCore(const Config& config, const EngineSettings& settings);
		~AetherCore();

		// Frame lifecycle.
		[[nodiscard]] bool ShouldClose() const;
		void PumpEvents() const;
		// Call once per frame before layer updates.
		// Updates input state and advances all non-manual cameras.
		void Tick(float dt);

		// Game-thread stage: flushes ECS draw data and snapshots immutable
		// render state for the render thread.
		[[nodiscard]] RenderFramePacket PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex);

		// Render-thread stage: records and submits Vulkan work for one frame.
		void ExecuteRenderFrame(const RenderFramePacket& packet);

		void WaitIdle() const;

		// Service accessors.
		[[nodiscard]] Renderer& GetRenderer();
		[[nodiscard]] const Renderer& GetRenderer() const;
		[[nodiscard]] AssetManager& GetAssets();
		[[nodiscard]] const AssetManager& GetAssets() const;
		[[nodiscard]] World& GetWorld();
		[[nodiscard]] const World& GetWorld() const;
		[[nodiscard]] Input& GetInput();
		[[nodiscard]] const Input& GetInput() const;
		[[nodiscard]] CameraManager& GetCameraManager();
		[[nodiscard]] const CameraManager& GetCameraManager() const;

		// Asset/resource creation.
		[[nodiscard]] const Mesh& GetPrimitiveMesh(PrimitiveMesh primitive) const;
		[[nodiscard]] GraphicsPipeline CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] Texture CreateTexture(std::string_view path, TextureFilter filter = TextureFilter::Linear);
		void RegisterMaterial(Material& mat);
		void UnregisterMaterial(Material& mat);
		[[nodiscard]] LoadedModel LoadModel(std::string_view path);
		[[nodiscard]] std::vector<Entity> SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale = 1.0f);

		// Voxel / chunk streaming support.
		// FlushMeshUploads submits all pending MeshUploadQueue copies via an
		// immediate submit before the next frame's draws run.
		[[nodiscard]] MeshArena& GetMeshArena();
		[[nodiscard]] MeshUploadQueue& GetMeshUploadQueue();
		void FlushMeshUploads();

		// Runtime rendering controls.
		void SetTonemapMode(TonemapMode mode);
		[[nodiscard]] TonemapMode GetTonemapMode() const;
		void SetFxaaEnabled(bool enabled);
		[[nodiscard]] bool IsFxaaEnabled() const;
		void SetDirectionalLight(glm::vec3 direction, float intensity);
		[[nodiscard]] glm::vec3 GetDirectionalLightDirection() const;
		[[nodiscard]] float GetDirectionalLightIntensity() const;
		void SetAmbientLight(glm::vec3 color);
		[[nodiscard]] glm::vec3 GetAmbientLight() const;
		void SetRttLightingBinningEnabled(bool enabled);
		[[nodiscard]] bool IsRttLightingBinningEnabled() const;
		void SetGpuLightingBinningEnabled(bool enabled);
		[[nodiscard]] bool IsGpuLightingBinningEnabled() const;

		// Format queries for app-side pipeline creation.
		[[nodiscard]] static constexpr VkFormat GetForwardColorFormat()
		{
			return PostProcessStack::GetForwardColorFormat();
		}

		// Swapchain properties.
		[[nodiscard]] VkExtent2D GetSwapchainExtent() const;
		// Depth format.
		[[nodiscard]] VkFormat GetSwapchainDepthFormat() const;
		// Color format.
		[[nodiscard]] VkFormat GetSwapchainImageFormat() const;
		// Current frame command buffer.
		[[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const;

		// Render target management.
		[[nodiscard]] CameraRenderTarget CreateCameraRenderTarget(CameraHandle camera, VkExtent2D extent);
		void DestroyCameraRenderTarget(CameraRenderTarget rt);
		[[nodiscard]] RGImage GetRenderTargetColorImage(CameraRenderTarget rt) const;
		[[nodiscard]] uint32_t GetRenderTargetBindlessSlot(CameraRenderTarget rt) const;

		// Engine internals for subsystems.
		[[nodiscard]] const VulkanContext& GetVulkanContext() const;
		[[nodiscard]] VulkanContext& GetVulkanContext();
		[[nodiscard]] const Swapchain& GetSwapchain() const;
		[[nodiscard]] Swapchain& GetSwapchain();
		[[nodiscard]] const BindlessManager& GetBindlessManager() const;
		[[nodiscard]] BindlessManager& GetBindlessManager();
		[[nodiscard]] const RenderGraph& GetRenderGraph() const;
		[[nodiscard]] RenderGraph& GetRenderGraph();
		[[nodiscard]] VkDescriptorSetLayout GetLightingSetLayout() const;
		[[nodiscard]] const RenderQueue& GetRenderQueue() const;
		[[nodiscard]] RenderQueue& GetRenderQueue();
		[[nodiscard]] const ShadowService& GetShadowService() const;
		[[nodiscard]] ShadowService& GetShadowService();
		[[nodiscard]] const ResourcePool& GetResourcePool() const;
		[[nodiscard]] ResourcePool& GetResourcePool();
		[[nodiscard]] const Scene& GetScene() const;
		[[nodiscard]] Scene& GetScene();
		[[nodiscard]] const Window& GetWindow() const;
		[[nodiscard]] Window& GetWindow();
		[[nodiscard]] const EngineSettings& GetSettings() const;

		// Invoked on the render thread after every swapchain recreation so callers
		// can re-register render graph passes that were cleared.
		void SetSwapchainRecreatedCallback(std::function<void(AetherCore&)> cb)
		{
			m_swapchainRecreatedCallback = std::move(cb);
		}

	private:
		// Frame graph and rendering.
		void BeginFrame();
		void EndFrame(const RenderFramePacket& packet);
		void RecreateSwapchain();
		void RegisterPasses();
		void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

		struct AsyncComputeFrame
		{
			VkCommandPool commandPool = VK_NULL_HANDLE;
			VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
			VkFence inFlight = VK_NULL_HANDLE;
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
		CommandRecorder m_currentRecorder; // Populated by BeginFrame.
		std::uint64_t m_frameIndex = 0;
		VkCommandPool m_uploadPool = VK_NULL_HANDLE;
		LightingManager m_lightingManager;
		std::array<AsyncComputeFrame, Swapchain::kMaxFramesInFlight> m_asyncComputeFrames{};
		VkSemaphore m_computeTimelineSemaphore = VK_NULL_HANDLE;
		std::uint64_t m_computeTimelineValue = 0;
		bool m_asyncComputeEnabled = false;
		EngineSettings m_settings{};
		std::function<void(AetherCore&)> m_swapchainRecreatedCallback;

		// Core services.
		Renderer m_renderer;
		AssetManager m_assetManager;
		MeshArena m_meshArena;
		MeshUploadQueue m_meshUploadQueue;

		// Offscreen/post-processing resources. Recreated on swapchain resize.
		PostProcessStack m_postProcessStack;
		SkyboxPass m_skyboxPass;
		CullPass m_cullPass;
		ForwardPass m_forwardPass;
		RenderPipelineCoordinator m_renderPipelineCoordinator;
		FrameComposer m_frameComposer;
		ShadowService m_shadowService;
		RenderTargetService m_renderTargetService;
		Input m_input;
		CameraManager m_cameraManager;
		MaterialBuffer m_materialBuffer;
	};
} // namespace aether
