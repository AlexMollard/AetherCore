#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "gpu/GpuFormat.hpp"
#include "gpu/GpuTypes.hpp"
#include "rendering/FrameConstantsBuffer.hpp"
#include "rendering/FrameContext.hpp"
#include "rendering/LightingManager.hpp"
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
	class EffectParamBuffer;
	class Renderer;
	class VulkanContext;
	class World;

	class RenderTargetService
	{
	public:
		void Initialize(VulkanContext& context, const RenderQueueSharedPipelines& pipelines);
		void Shutdown();

		void BindRuntime(const FrameContext& frame);

		void OnRenderGraphReset(gpu::Device device, gpu::Format depthFormat, gpu::Format forwardColorFormat);
		void RegisterPasses();

		void PrepareQueues(std::uint32_t drawSlot, World& world);
		void ClearAllQueues();
		void DiscardPendingQueues(std::uint32_t slot);
		void SetAnimationDatabase(const AnimationDatabase* animationDb);

		[[nodiscard]] Expected<std::uint32_t> CreateCameraRenderTarget(std::uint32_t cameraHandleRaw, gpu::Extent2D extent);
		void DestroyCameraRenderTarget(std::uint32_t id);
		[[nodiscard]] RGImage GetRenderTargetColorImage(std::uint32_t id) const;
		[[nodiscard]] std::uint32_t GetRenderTargetBindlessSlot(std::uint32_t id) const;
		[[nodiscard]] bool HasTarget(std::uint32_t id) const;

	private:
		struct Entry
		{
			std::uint32_t cameraHandleRaw = 0;
			gpu::Extent2D extent;
			std::string debugName;
			PreparedDrawList drawList{};
			RGImage rgColor{};
			RGImage rgDepth{};
			std::unique_ptr<FrameConstantsBuffer> constants;
			std::unique_ptr<RenderQueue> renderQueue;
			// Per-target light-binning view: local lights are culled against THIS
			// camera's frustum (registered lazily on first prepare).
			LightViewId lightViewId = kInvalidLightView;
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
		EffectParamBuffer* m_effectParamBuffer = nullptr;
		const CullPass* m_cullPass = nullptr;
		std::function<std::uint64_t()> m_getFrameIndex;
		gpu::Device m_device = nullptr;
		gpu::Format m_depthFormat = gpu::Format::Undefined;
		gpu::Format m_forwardColorFormat = gpu::Format::Undefined;
	};
} // namespace aether
