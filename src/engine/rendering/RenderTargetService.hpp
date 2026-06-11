#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include "vulkan/volk.hpp"

#include "gpu/GpuFormat.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderQueue.hpp"

namespace aether
{
	class AnimationDatabase;
	class BindlessManager;
	class CameraManager;
	class CullPass;
	class LightingManager;
	class MaterialBuffer;
	class Renderer;
	class Scene;
	class VulkanContext;
	class World;

	class RenderTargetService
	{
	public:
		void Initialize(VulkanContext& context, const RenderQueueSharedPipelines& pipelines);
		void Shutdown();

		void BindRuntime(const FrameContext& frame);

		void OnRenderGraphReset(VkDevice device, VkFormat depthFormat, VkFormat forwardColorFormat);
		void RegisterPasses();

		void PrepareQueues(std::uint32_t drawSlot, Scene& scene, World& world);
		void SetAnimationDatabase(const AnimationDatabase* animationDb);

		[[nodiscard]] Expected<std::uint32_t> CreateCameraRenderTarget(std::uint32_t cameraHandleRaw, VkExtent2D extent);
		void DestroyCameraRenderTarget(std::uint32_t id);
		[[nodiscard]] RGImage GetRenderTargetColorImage(std::uint32_t id) const;
		[[nodiscard]] std::uint32_t GetRenderTargetBindlessSlot(std::uint32_t id) const;
		[[nodiscard]] bool HasTarget(std::uint32_t id) const;

	private:
		struct Entry
		{
			std::uint32_t cameraHandleRaw = 0;
			VkExtent2D extent{};
			RGImage rgColor{};
			RGImage rgDepth{};
			std::unique_ptr<FrameConstantsBuffer> constants;
			RenderQueue renderQueue;
		};

		void RegisterPassFor(std::uint32_t id);

		std::unordered_map<std::uint32_t, Entry> m_targets;
		std::uint32_t m_nextId = 1;

		const RenderQueueSharedPipelines* m_sharedPipelines = nullptr;
		VulkanContext* m_context = nullptr;
		RenderGraph* m_graph = nullptr;
		BindlessManager* m_bindlessManager = nullptr;
		CameraManager* m_cameraManager = nullptr;
		LightingManager* m_lightingManager = nullptr;
		Renderer* m_renderer = nullptr;
		MaterialBuffer* m_materialBuffer = nullptr;
		const CullPass* m_cullPass = nullptr;
		std::function<std::uint64_t()> m_getFrameIndex;
		VkDevice m_device = VK_NULL_HANDLE;
		gpu::Format m_depthFormat = gpu::Format::Undefined;
		gpu::Format m_forwardColorFormat = gpu::Format::Undefined;
	};
} // namespace aether
