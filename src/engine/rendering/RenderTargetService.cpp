#include "RenderTargetService.hpp"
#include <glm/glm.hpp>
#include <stdexcept>
#include <string>
#include "gpu/GpuTypes.hpp"
#include "gpu/BindlessManager.hpp"
#include "camera/CameraManager.hpp"
#include "passes/CullPass.hpp"
#include "rendering/LightingManager.hpp"
#include "material/MaterialBuffer.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/Scene.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "scene/World.hpp"

namespace aether
{
	void RenderTargetService::Initialize(VulkanContext& context, const RenderQueueSharedPipelines& pipelines)
	{
		m_context = &context;
		m_sharedPipelines = &pipelines;
	}

	void RenderTargetService::Shutdown()
	{
		for (auto& [id, rt]: m_targets)
		{
			(void) id;
			rt.renderQueue.Shutdown();
			if (rt.constants)
			{
				rt.constants->Shutdown();
			}
		}
		m_targets.clear();
	}

	void RenderTargetService::BindRuntime(RenderGraph& graph,
	        BindlessManager& bindlessManager,
	        CameraManager& cameraManager,
	        LightingManager& lightingManager,
	        Renderer& renderer,
	        MaterialBuffer& materialBuffer,
	        const CullPass& cullPass,
	        std::function<std::uint64_t()> getFrameIndex,
	        const VkDevice device,
	        const VkFormat depthFormat,
	        const VkFormat forwardColorFormat)
	{
		m_graph = &graph;
		m_bindlessManager = &bindlessManager;
		m_cameraManager = &cameraManager;
		m_lightingManager = &lightingManager;
		m_renderer = &renderer;
		m_materialBuffer = &materialBuffer;
		m_cullPass = &cullPass;
		m_getFrameIndex = std::move(getFrameIndex);
		m_device = device;
		m_depthFormat = depthFormat;
		m_forwardColorFormat = forwardColorFormat;
	}

	void RenderTargetService::OnRenderGraphReset(const VkDevice device, const VkFormat depthFormat, const VkFormat forwardColorFormat)
	{
		m_device = device;
		m_depthFormat = depthFormat;
		m_forwardColorFormat = forwardColorFormat;

		if (m_graph == nullptr || m_bindlessManager == nullptr)
		{
			return;
		}

		for (auto& [id, rt]: m_targets)
		{
			rt.rgColor = m_graph->CreateTransientColor(m_forwardColorFormat, rt.extent, VK_IMAGE_USAGE_SAMPLED_BIT);
			const std::uint32_t slot = m_graph->EnsureBindlessSampled(rt.rgColor, *m_bindlessManager, m_device);
			if (slot == 0xFFFFFFFFu)
			{
				Throw(AetherError::Engine("RenderTargetService: failed to bindless-register transient RTT color for target id=" + std::to_string(id)));
			}
			rt.rgDepth = m_graph->CreateTransientDepth(m_depthFormat, rt.extent);
		}
	}

	void RenderTargetService::RegisterPasses()
	{
		for (const auto& [id, _]: m_targets)
		{
			(void) _;
			RegisterPassFor(id);
		}
	}

	void RenderTargetService::PrepareQueues(const std::uint32_t drawSlot, Scene& scene, World& world)
	{
		for (auto& [_, rt]: m_targets)
		{
			rt.renderQueue.SetWriteSlot(drawSlot);
			WorldRenderer::Flush(scene, rt.renderQueue);
			WorldRenderer::Flush(world, rt.renderQueue);
		}
	}

	void RenderTargetService::SetAnimationDatabase(const AnimationDatabase* animationDb)
	{
		for (auto& [_, rt]: m_targets)
		{
			rt.renderQueue.SetAnimationDatabase(animationDb);
		}
	}

	Expected<std::uint32_t> RenderTargetService::CreateCameraRenderTarget(const std::uint32_t cameraHandleRaw, const VkExtent2D extent)
	{
		AE_ASSERT_ALWAYS(m_context != nullptr && m_graph != nullptr && m_bindlessManager != nullptr, "RenderTargetService: runtime dependencies not bound before CreateCameraRenderTarget.");

		Entry rt{};
		rt.cameraHandleRaw = cameraHandleRaw;
		rt.extent = extent;

		rt.rgColor = m_graph->CreateTransientColor(m_forwardColorFormat, extent, VK_IMAGE_USAGE_SAMPLED_BIT);
		const std::uint32_t slot = m_graph->EnsureBindlessSampled(rt.rgColor, *m_bindlessManager, m_device);
		if (slot == 0xFFFFFFFFu)
		{
			AE_UNEXPECTED(AetherError::Engine("failed to register transient color image as bindless sampled"));
		}
		rt.rgDepth = m_graph->CreateTransientDepth(m_depthFormat, extent);

		rt.constants = std::make_unique<FrameConstantsBuffer>();
		rt.constants->Initialize(*m_context);
		rt.renderQueue.Initialize(m_context->GetDevice().device, m_context->GetAllocator(), *m_sharedPipelines);
		rt.renderQueue.SetTracyVkCtx(m_context->GetTracyVkCtx());

		const std::uint32_t id = m_nextId++;
		m_targets.emplace(id, std::move(rt));
		RegisterPassFor(id);
		return id;
	}

	void RenderTargetService::DestroyCameraRenderTarget(const std::uint32_t id)
	{
		if (id == 0 || m_graph == nullptr)
		{
			return;
		}

		auto it = m_targets.find(id);
		if (it == m_targets.end())
		{
			return;
		}

		m_graph->RemovePass("$CameraRT_" + std::to_string(id));
		m_graph->RemovePass("$CullDraws_RTT_" + std::to_string(id));
		m_graph->ReleaseImage(it->second.rgColor);
		m_graph->ReleaseImage(it->second.rgDepth);

		it->second.renderQueue.Shutdown();
		if (it->second.constants)
		{
			it->second.constants->Shutdown();
		}

		m_targets.erase(it);
	}

	RGImage RenderTargetService::GetRenderTargetColorImage(const std::uint32_t id) const
	{
		auto it = m_targets.find(id);
		if (it == m_targets.end())
		{
			return {};
		}
		return it->second.rgColor;
	}

	std::uint32_t RenderTargetService::GetRenderTargetBindlessSlot(const std::uint32_t id) const
	{
		if (m_graph == nullptr)
		{
			return 0xFFFFFFFFu;
		}
		auto it = m_targets.find(id);
		if (it == m_targets.end())
		{
			return 0xFFFFFFFFu;
		}
		return m_graph->GetBindlessSampledSlot(it->second.rgColor);
	}

	bool RenderTargetService::HasTarget(const std::uint32_t id) const
	{
		return m_targets.find(id) != m_targets.end();
	}

	void RenderTargetService::RegisterPassFor(const std::uint32_t id)
	{
		if (m_graph == nullptr || m_cameraManager == nullptr || m_lightingManager == nullptr || m_renderer == nullptr || m_materialBuffer == nullptr || m_cullPass == nullptr || m_bindlessManager == nullptr)
		{
			return;
		}

		auto it = m_targets.find(id);
		if (it == m_targets.end())
		{
			return;
		}

		const std::string idStr = std::to_string(id);
		const RGImage color = it->second.rgColor;
		const RGImage depth = it->second.rgDepth;
		const VkExtent2D extent = it->second.extent;
		const VkPipeline cullPipeline = m_cullPass->GetSinglePipeline();
		const VkPipelineLayout cullLayout = m_cullPass->GetSingleLayout();

		m_graph->AddComputePass("$CullDraws_RTT_" + idStr)
		        .ExecuteCompute(
		                [this, id, cullPipeline, cullLayout](PassContext& ctx)
		                {
			                auto rit = m_targets.find(id);
			                if (rit == m_targets.end())
			                {
				                return;
			                }

			                Camera* cam = m_cameraManager->TryGet(CameraHandle{rit->second.cameraHandleRaw});
			                if (cam == nullptr || !rit->second.constants)
			                {
				                return;
			                }

			                const float aspect = static_cast<float>(rit->second.extent.width) / static_cast<float>(rit->second.extent.height);

			                FrameConstants fc{};
			                fc.view = cam->GetViewMatrix();
			                fc.proj = cam->GetProjectionMatrix(aspect);
			                fc.viewProj = fc.proj * fc.view;
			                fc.cameraWorldPos = glm::vec4(cam->GetPosition(), 1.0f);
			                fc.materialBufferAddr = m_materialBuffer->GetDeviceAddressU64();
			                fc.sunDirectionIntensity = m_renderer->GetDirectionalLightVector();
			                fc.ambientColor = m_renderer->GetAmbientLightVector();
			                fc.sunColor = m_renderer->GetSunColorVector();
			                fc.skyHorizonColor = m_renderer->GetSkyHorizonColorVector();
			                fc.skyZenithColor = m_renderer->GetSkyZenithColorVector();
			                fc.skyVoidColor = m_renderer->GetSkyVoidColorVector();

			                const auto frameIdx = static_cast<std::uint32_t>(m_getFrameIndex() % aether::kMaxFramesInFlight);
			                CommandRecorder nullRecorder;
			                m_lightingManager->UpdateForView(frameIdx, nullRecorder, *cam, GpuExtent2D(rit->second.extent), fc, m_lightingManager->IsRttBinningEnabled());
			                rit->second.constants->Write(frameIdx, fc);
			                const gpu::DeviceAddress frameAddr = rit->second.constants->GetDeviceAddress(frameIdx);

			                rit->second.renderQueue.PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), frameAddr, cullPipeline, cullLayout, ctx.frameIndex);
		                });

		m_graph->AddPass("$CameraRT_" + idStr)
		        .WriteColor(color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearColorValue(0.02f, 0.02f, 0.03f, 1.0f))
		        .WriteDepth(depth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, ClearDepthValue(1.0f))
		        .SetExtent(extent)
		        .Execute(
		                [this, id](PassContext& ctx)
		                {
			                auto rit = m_targets.find(id);
			                if (rit == m_targets.end())
			                {
				                return;
			                }
			                if (!rit->second.constants)
			                {
				                return;
			                }

			                const auto frameIdx = static_cast<std::uint32_t>(m_getFrameIndex() % Swapchain::kMaxFramesInFlight);
			                auto pushLighting = [this, frameIdx](VkCommandBuffer cmd, VkPipelineLayout layout)
			                {
				                m_lightingManager->PushLightingDescriptor(cmd, layout, frameIdx);
			                };
			                rit->second.renderQueue.FlushDrawPush(ctx.recorder, m_bindlessManager->GetSet(), pushLighting);
			                rit->second.renderQueue.Clear(static_cast<std::uint32_t>(ctx.frameIndex % RenderQueue::kFramesInFlight));
		                });
	}
} // namespace aether
