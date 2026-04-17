#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "AnimationDatabase.hpp"
#include "AssetManager.hpp"
#include "BindlessManager.hpp"
#include "Camera.hpp"
#include "CameraManager.hpp"
#include "CommandRecorder.hpp"
#include "CullPass.hpp"
#include "EngineSettings.hpp"
#include "ForwardPass.hpp"
#include "FrameConstants.hpp"
#include "FrameConstantsBuffer.hpp"
#include "GraphicsPipeline.hpp"
#include "Input.hpp"
#include "LightingManager.hpp"
#include "Material.hpp"
#include "MaterialBuffer.hpp"
#include "Mesh.hpp"
#include "ModelAnimator.hpp"
#include "PostProcessStack.hpp"
#include "PrimitiveMeshes.hpp"
#include "Renderer.hpp"
#include "RenderGraph.hpp"
#include "RenderQueue.hpp"
#include "RenderThread.hpp"
#include "ResourcePool.hpp"
#include "Scene.hpp"
#include "SkyboxPass.hpp"
#include "Swapchain.hpp"
#include "Texture.hpp"
#include "UniqueBuffer.hpp"
#include "UniqueImage.hpp"
#include "VulkanContext.hpp"
#include "Window.hpp"
#include "World.hpp"

namespace aether
{
	struct LoadedModelPrimitive
	{
		Mesh mesh;
		Material material{};
		glm::mat4 localTransform{ 1.0f }; // identity for skinned primitives
		std::int32_t skinIndex = -1;      // -1 = not skinned
	};

	struct LoadedModel
	{
		std::vector<Texture> textures;
		std::vector<LoadedModelPrimitive> primitives;
		std::optional<ModelAnimator> animator;
		AnimationDatabase animationDb; // GPU-friendly animation clip data (if model has animations)
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
			const char* settingsFile = "engine.ini";
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
		~AetherCore();

		// ── Frame lifecycle ──────────────────────────────────────────────────
		[[nodiscard]] bool ShouldClose() const;
		void PumpEvents() const;
		// Must be called once per frame BEFORE layer OnUpdate().
		// Updates input state and advances all non-Manual cameras.
		void Tick(float dt);

		// Game-thread: flush ECS draw commands into the double-buffered slot and
		// snapshot per-frame render state into a packet for the render thread.
		// Call AFTER LayerGui (so UI draw calls are captured), BEFORE SubmitFrame.
		[[nodiscard]] RenderFramePacket PrepareFrame(std::uint32_t drawSlot, std::uint64_t frameIndex);

		// Render-thread: perform all Vulkan work for a single frame.
		// Called exclusively by RenderThread::ThreadLoop.
		void ExecuteRenderFrame(const RenderFramePacket& packet);

		void WaitIdle() const;

		// ── Service accessors ────────────────────────────────────────────────
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

		// ── Asset creation (used by app layer or engine internals)
		// ──────────────────
		[[nodiscard]] const Mesh& GetPrimitiveMesh(PrimitiveMesh primitive) const;
		[[nodiscard]] GraphicsPipeline CreateGraphicsPipeline(const GraphicsPipeline::Desc& desc);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices);
		[[nodiscard]] Mesh CreateMesh(std::span<const Mesh::Vertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] Texture CreateTexture(std::string_view path);
		void RegisterMaterial(Material& mat);
		void UnregisterMaterial(Material& mat);
		[[nodiscard]] LoadedModel LoadModel(std::string_view path);
		[[nodiscard]] std::vector<Entity> SpawnModel(LoadedModel& model, GraphicsPipeline& pipeline, float scale = 1.0f);

		// ── Rendering controls (used by app layer or engine internals)
		// ──────────────────
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

		// ── Format queries for app-layer pipeline creation ──────────────────
		// Query the forward pass color format.
		[[nodiscard]] static constexpr VkFormat GetForwardColorFormat()
		{
			return PostProcessStack::GetForwardColorFormat();
		}

		// Query swapchain extent.
		[[nodiscard]] VkExtent2D GetSwapchainExtent() const;
		// Query swapchain depth format.
		[[nodiscard]] VkFormat GetSwapchainDepthFormat() const;
		// Query swapchain image format.
		[[nodiscard]] VkFormat GetSwapchainImageFormat() const;
		// Get current swapchain command buffer (used by engine internals).
		[[nodiscard]] VkCommandBuffer GetCurrentCommandBuffer() const;

		// ── Render target management ─────────────────────────────────────────
		[[nodiscard]] CameraRenderTarget CreateCameraRenderTarget(CameraHandle camera, VkExtent2D extent);
		void DestroyCameraRenderTarget(CameraRenderTarget rt);
		[[nodiscard]] RGImage GetRenderTargetColorImage(CameraRenderTarget rt) const;
		[[nodiscard]] uint32_t GetRenderTargetBindlessSlot(CameraRenderTarget rt) const;

		// ── Engine internals (used by engine subsystems, not app layer) ──────
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
		[[nodiscard]] const ResourcePool& GetResourcePool() const;
		[[nodiscard]] ResourcePool& GetResourcePool();
		[[nodiscard]] const Scene& GetScene() const;
		[[nodiscard]] Scene& GetScene();
		[[nodiscard]] const Window& GetWindow() const;
		[[nodiscard]] Window& GetWindow();
		[[nodiscard]] const EngineSettings& GetSettings() const;

	private:
		// ── Frame graph and rendering ───────────────────────────────────────
		void BeginFrame();                              // called from ExecuteRenderFrame on render thread
		void EndFrame(const RenderFramePacket& packet); // called from ExecuteRenderFrame on render thread
		void RecreateSwapchain();
		void RegisterPasses();
		void RegisterRttPassesFor(uint32_t id);
		void ImmediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

		// ── Per-camera render-to-texture entry ───────────────────────────────
		struct CameraRtEntry
		{
			CameraHandle camera;
			VkExtent2D extent;
			RGImage rgColor{};
			RGImage rgDepth{};
			std::unique_ptr<FrameConstantsBuffer> constants;
			RenderQueue renderQueue; // own queue — isolated from main m_renderQueue
		};

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
		CommandRecorder m_currentRecorder; // engine-internal, filled by BeginFrame
		std::uint64_t m_frameIndex = 0;
		VkCommandPool m_uploadPool = VK_NULL_HANDLE;
		LightingManager m_lightingManager;
		std::array<AsyncComputeFrame, Swapchain::kMaxFramesInFlight> m_asyncComputeFrames{};
		VkSemaphore m_computeTimelineSemaphore = VK_NULL_HANDLE;
		std::uint64_t m_computeTimelineValue = 0;
		bool m_asyncComputeEnabled = false;
		EngineSettings m_settings{};

		// ── Services ─────────────────────────────────────────────────────────
		Renderer m_renderer;
		AssetManager m_assetManager;

		// Manages all offscreen targets and post-processing pipelines.
		// Recreated on swapchain resize.
		PostProcessStack m_postProcessStack;
		SkyboxPass m_skyboxPass;
		CullPass m_cullPass;
		ForwardPass m_forwardPass;
		Input m_input;
		CameraManager m_cameraManager;
		MaterialBuffer m_materialBuffer;

		std::unordered_map<uint32_t, CameraRtEntry> m_rtCameras;
		uint32_t m_nextRtId = 1;
	};
} // namespace aether
