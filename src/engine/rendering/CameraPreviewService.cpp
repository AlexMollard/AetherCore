#include "rendering/CameraPreviewService.hpp"

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "passes/CullPass.hpp"
#include "rendering/GpuContracts.hpp"
#include "rendering/LightingManager.hpp"
#include "rendering/RenderGraphTypes.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether
{
	void CameraPreviewService::Initialize(VulkanContext& context, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines, gpu::Format colorFormat, gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		(void) context;
		(void) bindless;
		m_colorFormat = colorFormat;
		m_depthFormat = depthFormat;

		// A small, self-contained queue mirroring the scene draws for the preview POV.
		m_queue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 8192u, .debugName = "CameraPreview"});
		m_constants.Initialize();

		// HDR colour target (forward format so the scene's material pipelines can draw
		// into it) + depth. Bindless-sample the colour so a UI panel can show it.
		m_colorHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = colorFormat,
		        .extent = {kWidth, kHeight},
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "CameraPreview.Color",
		});
		m_depthHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = depthFormat,
		        .extent = {kWidth, kHeight},
		        .usage = gpu::ImageUsage::DepthStencilAttachment,
		        .aspect = gpu::ImageAspect::Depth,
		        .debugName = "CameraPreview.Depth",
		});
		if (m_colorHandle.IsValid())
		{
			m_colorView = gpu::ResourceRegistry::ResolveTexture(m_colorHandle).view;
			gpu::ResourceRegistry::EnsureBindlessSampled(m_colorHandle, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
			m_colorBindlessSlot = gpu::ResourceRegistry::GetBindlessSampledSlot(m_colorHandle);
		}

		m_initialized = m_colorHandle.IsValid() && m_depthHandle.IsValid();
	}

	void CameraPreviewService::Shutdown()
	{
		m_queue.DiscardAllPending();
		m_constants.Shutdown();
		if (m_colorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_colorHandle);
		}
		if (m_depthHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_depthHandle);
		}
		m_colorHandle = {};
		m_depthHandle = {};
		m_colorView = nullptr;
		m_colorBindlessSlot = 0xFFFFFFFFu;
		m_initialized = false;
	}

	void CameraPreviewService::SetRequest(bool enabled, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cameraPos)
	{
		{
			std::lock_guard<std::mutex> lock(m_requestMutex);
			m_reqView = view;
			m_reqProj = proj;
			m_reqCameraPos = cameraPos;
		}
		m_enabled.store(enabled, std::memory_order_relaxed);
	}

	void CameraPreviewService::PrepareQueue(std::uint32_t drawSlot, World& world)
	{
		if (!m_initialized)
		{
			return;
		}
		// Always prepare + (below) let the cull consume the slot so the producer/
		// consumer lifecycle stays balanced every frame; only pay the mesh walk when
		// the preview is actually on.
		m_queue.SetWriteSlot(drawSlot);
		m_queue.Clear(drawSlot);
		if (m_enabled.load(std::memory_order_relaxed))
		{
			WorldRenderer::Flush(world, m_queue, /*shadowPass*/ false);
		}
	}

	void CameraPreviewService::BuildFrameConstants(const FrameConstants& mainFc, std::uint32_t frameIdx)
	{
		if (!m_initialized)
		{
			return;
		}
		// Copy the main frame constants (keeps lighting / shadow / resource-table
		// addresses valid) and override only the camera so culling + shading run from
		// the preview POV.
		FrameConstants fc = mainFc;
		{
			std::lock_guard<std::mutex> lock(m_requestMutex);
			fc.view = m_reqView;
			fc.proj = m_reqProj;
			fc.viewProj = m_reqProj * m_reqView;
			fc.cameraWorldPos = glm::vec4(m_reqCameraPos, 1.0f);
		}
		fc.RefreshDerived(); // preview frustum planes + invViewProj
		m_constants.Write(frameIdx, fc);
	}

	void CameraPreviewService::RegisterImages(RenderGraph& graph)
	{
		if (!m_initialized)
		{
			return;
		}
		m_color = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_colorHandle), m_colorView, gpu::ImageAspect::Color);
		const auto depthTex = gpu::ResourceRegistry::ResolveTexture(m_depthHandle);
		m_depth = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_depthHandle), depthTex.view, gpu::ImageAspect::Depth);
	}

	void CameraPreviewService::RegisterComputePasses(RenderGraph& graph, CullPass& cullPass)
	{
		if (!m_initialized)
		{
			return;
		}
		RegisterImages(graph);
		m_draws = graph.CreatePreparedDrawList("CameraPreviewDraws");

		// Cull the preview queue against the PREVIEW frustum (its own frame
		// constants), producing the preview draw list. Always registered; when the
		// preview is off it just discards the (empty) queue slot.
		graph.AddQueuePreparePass({
		             .name = "$CameraPreviewCull",
		             .produces = m_draws,
		             .sideEffectReason = "prepares camera-preview draw queue",
		     })
		        .ExecuteCompute(
		                [this, &cullPass](PassContext& ctx)
		                {
			                // Always dispatch so the slot is consumed each frame (the
			                // queue is empty when the preview is off => 0 draws).
			                m_queue.PrepareAndDispatch(ctx.recorder, m_constants.GetDeviceAddress(ctx.frameSlot), cullPass.GetSinglePipeline(), ctx.frameSlot);
		                })
		        .OnDebugDisabled([this](PassContext& ctx) { m_queue.DiscardPending(ctx.frameSlot); });
	}

	void CameraPreviewService::RegisterGraphicsPasses(RenderGraph& graph, LightingManager* lighting, BindlessManager& bindless)
	{
		if (!m_initialized)
		{
			return;
		}
		// Lit forward pass from the preview POV into the preview colour+depth. Depends
		// on the shadow atlases + light buffers so they are produced first; the
		// per-draw material pipelines shade using the preview frame constants (which
		// carry the same lighting/shadow/resource-table addresses as the main view).
		std::vector<RenderGraph::FrameProductRef> consumes;
		if (lighting != nullptr)
		{
			consumes.push_back(RenderGraph::Product<LightBuffersProduct>(kFrameProductLightBuffers));
		}
		auto pass = graph.AddDrawQueuePass({
		        .name = "$CameraPreviewForward",
		        .color = m_color,
		        .depth = m_depth,
		        .draws = m_draws,
		        .extent = {kWidth, kHeight},
		        .colorLoadOp = gpu::LoadOp::Clear,
		        .depthLoadOp = gpu::LoadOp::Clear,
		        .consumes = std::move(consumes),
		});
		pass.ConsumeTextureProduct<FrameTextureArrayProduct>(kFrameProductDirectionalShadows, FrameResourceId::DirectionalShadowC0);
		pass.ConsumeTextureProduct<LocalShadowProduct>(kFrameProductLocalShadows, FrameResourceId::LocalShadowAtlas);
		if (lighting != nullptr)
		{
			pass.ReadBuffer(lighting->GetLightsBufferHandle());
			pass.ReadBuffer(lighting->GetTileHeadersBufferHandle());
			pass.ReadBuffer(lighting->GetTileIndicesBufferHandle());
		}
		pass.Execute(
		        [this, lighting, &bindless](PassContext& ctx)
		        {
			        if (!m_enabled.load(std::memory_order_relaxed))
			        {
				        return; // colour/depth still clear to black
			        }
			        const DrawContracts::LightingAddresses lightingAddr = lighting != nullptr ? lighting->GetLightingAddresses(ctx.frameSlot) : DrawContracts::LightingAddresses{};
			        bindless.CmdBindHeaps(ctx.recorder);
			        m_queue.FlushDrawWithFrameAddr(ctx.recorder, ctx.frameSlot, &lightingAddr, m_constants.GetDeviceAddress(ctx.frameSlot), nullptr, 0, nullptr);
		        });

		// A no-op read leaves the colour ShaderReadOnly so a UI panel can sample it.
		graph.AddPass("$CameraPreviewReady").ReadTexture(m_color).Execute([](PassContext&) {});
	}
} // namespace aether
