#include "RenderTargetService.hpp"
#include <glm/glm.hpp>
#include <stdexcept>
#include <string>
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/BindlessManager.hpp"
#include "camera/CameraManager.hpp"
#include "passes/CullPass.hpp"
#include "rendering/LightingManager.hpp"
#include "material/MaterialBuffer.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/WorldRenderer.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void RenderTargetService::Initialize(VulkanContext& context, const RenderQueueSharedPipelines& pipelines)
	{
		AE_PROFILE_ZONE();
		m_context = &context;
		m_sharedPipelines = &pipelines;
	}

	void RenderTargetService::Shutdown()
	{
		AE_PROFILE_ZONE();
		for (auto& [id, rt]: m_targets)
		{
			(void) id;
			rt.renderQueue->Shutdown();
			if (rt.constants)
			{
				rt.constants->Shutdown();
			}
		}
		m_targets.clear();
	}

	void RenderTargetService::BindRuntime(const FrameContext& frame)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(frame.graph != nullptr, "RenderTargetService::BindRuntime: frame.graph is null");
		AE_ASSERT(frame.bindless != nullptr, "RenderTargetService::BindRuntime: frame.bindless is null");
		AE_ASSERT(frame.cameras != nullptr, "RenderTargetService::BindRuntime: frame.cameras is null");
		AE_ASSERT(frame.lighting != nullptr, "RenderTargetService::BindRuntime: frame.lighting is null");
		AE_ASSERT(frame.renderer != nullptr, "RenderTargetService::BindRuntime: frame.renderer is null");
		AE_ASSERT(frame.materials != nullptr, "RenderTargetService::BindRuntime: frame.materials is null");
		AE_ASSERT(frame.cullPass != nullptr, "RenderTargetService::BindRuntime: frame.cullPass is null");
		AE_ASSERT(frame.frameIndex, "RenderTargetService::BindRuntime: frame.frameIndex is empty");
		AE_ASSERT(m_context != nullptr, "RenderTargetService::BindRuntime: Initialize() was not called");

		m_graph = frame.graph;
		m_bindlessManager = frame.bindless;
		m_cameraManager = frame.cameras;
		m_lightingManager = frame.lighting;
		m_renderer = frame.renderer;
		m_materialBuffer = frame.materials;
		m_cullPass = frame.cullPass;
		m_getFrameIndex = frame.frameIndex;
		m_device = static_cast<gpu::Device>(m_context->GetDevice().device);
		m_depthFormat = frame.depthFormat;
		m_forwardColorFormat = frame.colorFormat;
	}

	void RenderTargetService::OnRenderGraphReset(const gpu::Device device, const gpu::Format depthFormat, const gpu::Format forwardColorFormat)
	{
		AE_PROFILE_ZONE();
		m_device = device;
		m_depthFormat = depthFormat;
		m_forwardColorFormat = forwardColorFormat;

		if (m_graph == nullptr || m_bindlessManager == nullptr)
		{
			return;
		}

		for (auto& [id, rt]: m_targets)
		{
			rt.rgColor = m_graph->CreateTransientColor(m_forwardColorFormat, gpu::Extent2D{rt.extent.width, rt.extent.height}, gpu::ImageUsage::Sampled);
			const std::uint32_t slot = m_graph->EnsureBindlessSampled(rt.rgColor);
			if (slot == 0xFFFFFFFFu)
			{
				Throw(AetherError::Engine("RenderTargetService: failed to bindless-register transient RTT color for target id=" + std::to_string(id)));
			}
			rt.rgDepth = m_graph->CreateTransientDepth(m_depthFormat, gpu::Extent2D{rt.extent.width, rt.extent.height});
		}
	}

	void RenderTargetService::RegisterPasses()
	{
		AE_PROFILE_ZONE();
		for (const auto& [id, _]: m_targets)
		{
			(void) _;
			RegisterPassFor(id);
		}
	}

	void RenderTargetService::PrepareQueues(const std::uint32_t drawSlot, World& world)
	{
		AE_PROFILE_ZONE();
		for (auto& [_, rt]: m_targets)
		{
			rt.renderQueue->SetWriteSlot(drawSlot);
			rt.renderQueue->Clear(drawSlot);
			WorldRenderer::Flush(world, *rt.renderQueue);
		}
	}

	void RenderTargetService::ClearAllQueues()
	{
		AE_PROFILE_ZONE();
		for (auto& [_, rt]: m_targets)
		{
			if (rt.renderQueue)
			{
				rt.renderQueue->DiscardAllPending();
			}
		}
	}

	void RenderTargetService::DiscardPendingQueues(const std::uint32_t slot)
	{
		AE_PROFILE_ZONE();
		for (auto& [_, rt]: m_targets)
		{
			if (rt.renderQueue)
			{
				rt.renderQueue->DiscardPending(slot);
			}
		}
	}

	void RenderTargetService::SetAnimationDatabase(const AnimationDatabase* animationDb)
	{
		for (auto& [_, rt]: m_targets)
		{
			rt.renderQueue->SetAnimationDatabase(animationDb);
		}
	}

	Expected<std::uint32_t> RenderTargetService::CreateCameraRenderTarget(const std::uint32_t cameraHandleRaw, const gpu::Extent2D extent)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT_ALWAYS(m_context != nullptr && m_graph != nullptr && m_bindlessManager != nullptr, "RenderTargetService: runtime dependencies not bound before CreateCameraRenderTarget.");

		const std::uint32_t id = m_nextId++;
		Entry rt{};
		rt.cameraHandleRaw = cameraHandleRaw;
		rt.extent = extent;
		rt.debugName = "CameraRT_" + std::to_string(id);

		rt.rgColor = m_graph->CreateTransientColor(m_forwardColorFormat, extent, gpu::ImageUsage::Sampled);
		const std::uint32_t slot = m_graph->EnsureBindlessSampled(rt.rgColor);
		if (slot == 0xFFFFFFFFu)
		{
			AE_UNEXPECTED(AetherError::Engine("failed to register transient color image as bindless sampled"));
		}
		rt.rgDepth = m_graph->CreateTransientDepth(m_depthFormat, extent);

		rt.constants = std::make_unique<FrameConstantsBuffer>();
		rt.constants->Initialize();
		rt.renderQueue = std::make_unique<RenderQueue>();
		rt.renderQueue->Initialize(*m_sharedPipelines, RenderQueueConfig{.debugName = rt.debugName.c_str()});

		m_targets.emplace(id, std::move(rt));
		RegisterPassFor(id);
		return id;
	}

	void RenderTargetService::DestroyCameraRenderTarget(const std::uint32_t id)
	{
		AE_PROFILE_ZONE();
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

		it->second.renderQueue->Shutdown();
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
		const gpu::Extent2D extent = it->second.extent;
		const gpu::Pipeline cullPipeline = m_cullPass->GetSinglePipeline();

		m_graph->AddComputePass("$CullDraws_RTT_" + idStr)
		        .ExecuteCompute(
		                [this, id, cullPipeline](PassContext& ctx)
		                {
			                auto rit = m_targets.find(id);
			                if (rit == m_targets.end())
			                {
				                return;
			                }

			                Camera* cam = m_cameraManager->TryGet(CameraHandle{rit->second.cameraHandleRaw});
			                if (cam == nullptr || !rit->second.constants)
			                {
				                rit->second.renderQueue->DiscardPending(ctx.frameIndex);
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
			                m_lightingManager->UpdateForView(frameIdx, *cam, gpu::Extent2D(rit->second.extent), fc, m_lightingManager->IsRttBinningEnabled());
			                rit->second.constants->Write(frameIdx, fc);
			                const gpu::DeviceAddress frameAddr = rit->second.constants->GetDeviceAddress(frameIdx);

			                rit->second.renderQueue->PrepareAndDispatch(ctx.recorder, frameAddr, cullPipeline, ctx.frameIndex);
		                })
		        .OnDebugDisabled(
		                [this, id](PassContext& ctx)
		                {
			                if (auto rit = m_targets.find(id); rit != m_targets.end())
			                {
				                rit->second.renderQueue->DiscardPending(ctx.frameIndex);
			                }
		                });

		m_graph->AddPass("$CameraRT_" + idStr)
		        .WriteColor(color, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(0.02f, 0.02f, 0.03f, 1.0f))
		        .WriteDepth(depth, gpu::LoadOp::Clear, gpu::StoreOp::DontCare, ClearDepthValue(1.0f))
		        .SetExtent(gpu::Extent2D{extent.width, extent.height})
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
			                auto lightingAddr = m_lightingManager ? m_lightingManager->GetLightingAddresses(frameIdx) : DrawContracts::LightingAddresses{};
			                gpu::CommandList cmd = ctx.recorder.View();
			                m_bindlessManager->CmdBindHeaps(cmd);
			                rit->second.renderQueue->FlushDrawPush(cmd, lightingAddr);
		                });
	}
} // namespace aether
