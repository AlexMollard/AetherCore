#include "rendering/CameraPreviewService.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "passes/CullPass.hpp"
#include "passes/PostProcessStack.hpp"
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

		// LDR resolve target: the preview tonemap pass writes here (matching the main
		// view exposure + operator) and ImGui samples it. R8 so it is display-ready and
		// matches the reused tonemap pipeline colour format.
		m_colorLdrHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = gpu::Format::R8G8B8A8Unorm,
		        .extent = {kWidth, kHeight},
		        .usage = gpu::ImageUsage::ColorAttachment | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "CameraPreview.ColorLdr",
		});
		if (m_colorLdrHandle.IsValid())
		{
			m_colorLdrView = gpu::ResourceRegistry::ResolveTexture(m_colorLdrHandle).view;
		}

		m_initialized = m_colorHandle.IsValid() && m_depthHandle.IsValid() && m_colorLdrHandle.IsValid();
	}

	void CameraPreviewService::Shutdown()
	{
		m_queue.DiscardAllPending();
		m_queue.Shutdown(); // release the queue's persistent GPU buffers now, while the ResourceRegistry is alive - else they leak to ResourceRegistry::Shutdown
		m_constants.Shutdown();
		if (m_colorHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_colorHandle);
		}
		if (m_depthHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_depthHandle);
		}
		if (m_colorLdrHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_colorLdrHandle);
		}
		if (m_lighting != nullptr && m_lightView != kInvalidLightView)
		{
			m_lighting->UnregisterView(m_lightView);
		}
		m_lighting = nullptr;
		m_lightView = kInvalidLightView;
		m_colorHandle = {};
		m_depthHandle = {};
		m_colorLdrHandle = {};
		m_colorView = nullptr;
		m_colorLdrView = nullptr;
		m_colorBindlessSlot = 0xFFFFFFFFu;
		m_initialized = false;
	}

	void CameraPreviewService::SetRequest(bool enabled, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cameraPos, float nearPlane)
	{
		{
			std::lock_guard<std::mutex> lock(m_requestMutex);
			m_reqView = view;
			m_reqProj = proj;
			m_reqCameraPos = cameraPos;
			m_reqNearPlane = nearPlane;
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

	void CameraPreviewService::BuildFrameConstants(const FrameConstants& mainFc, std::uint32_t frameIdx, LightingManager* lighting)
	{
		if (!m_initialized)
		{
			return;
		}
		// Copy the main frame constants (keeps shadow / resource-table addresses
		// valid) and override only the camera so culling + shading run from the
		// preview POV.
		FrameConstants fc = mainFc;
		float nearPlane = 0.1f;
		{
			std::lock_guard<std::mutex> lock(m_requestMutex);
			fc.view = m_reqView;
			fc.proj = m_reqProj;
			fc.viewProj = m_reqProj * m_reqView;
			fc.cameraWorldPos = glm::vec4(m_reqCameraPos, 1.0f);
			nearPlane = m_reqNearPlane;
		}

		// Bin local lights against the PREVIEW frustum: register a light view on
		// first use and stage its dispatch for the shared $Lighting.BinLights pass
		// (runs after the main-view prepare, so the frame's light list is uploaded).
		if (lighting != nullptr && m_lightView == kInvalidLightView)
		{
			m_lightView = lighting->RegisterView("CameraPreview");
			m_lighting = lighting;
		}
		const bool lit = lighting != nullptr && m_lightView != kInvalidLightView && m_enabled.load(std::memory_order_relaxed)
		        && lighting->PrepareView(m_lightView, frameIdx, fc.view, fc.proj, nearPlane, {kWidth, kHeight}, fc);
		if (!lit)
		{
			// Hide the main view's grid info the fc copy carried: its tile lists are
			// only valid for the main camera.
			fc.tiledLightGridInfo = glm::uvec4(0u);
			fc.tiledLightBufferOffsets = glm::uvec4(0u);
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
		m_colorLdr = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_colorLdrHandle), m_colorLdrView, gpu::ImageAspect::Color);
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

	void CameraPreviewService::RegisterGraphicsPasses(RenderGraph& graph, LightingManager* lighting, BindlessManager& bindless, const PostProcessStack& postProcess, gpu::Pipeline skyboxPipeline)
	{
		if (!m_initialized)
		{
			return;
		}

		// Sky first, from the preview POV, so the thumbnail shares the main view's
		// backdrop instead of black. Fullscreen + no depth (mirrors the main $Skybox);
		// the forward pass then Loads over it. Pushes the PREVIEW frame constants so the
		// sky is rendered from the preview camera (its invViewProj drives the rays).
		graph
		        .AddFullscreenPass({
		                .name = "$CameraPreviewSkybox",
		                .color = m_color,
		                .extent = {kWidth, kHeight},
		                .loadOp = gpu::LoadOp::Clear,
		        })
		        .Execute(
		                [this, skyboxPipeline](PassContext& ctx)
		                {
			                if (!m_enabled.load(std::memory_order_relaxed))
			                {
				                return; // colour stays cleared to black
			                }
			                gpu::CommandList& cmd = ctx.recorder;
			                cmd.BindPipeline(skyboxPipeline);
			                const gpu::DeviceAddress frameAddr = m_constants.GetDeviceAddress(ctx.frameSlot);
			                std::byte bytes[sizeof(gpu::DeviceAddress)];
			                std::memcpy(bytes, &frameAddr, sizeof(bytes));
			                cmd.PushDataRaw(0, std::span<const std::byte>(bytes, sizeof(bytes)));
			                cmd.Draw(3);
		                });

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
		        .colorLoadOp = gpu::LoadOp::Load, // keep the sky the skybox pass drew
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
			        // Shade with the preview view's own tile lists (culled against the
			        // preview frustum by $Lighting.BinLights).
			        const DrawContracts::LightingAddresses lightingAddr = lighting != nullptr && m_lightView != kInvalidLightView
			                ? lighting->GetLightingAddresses(m_lightView, ctx.frameSlot)
			                : DrawContracts::LightingAddresses{};
			        bindless.CmdBindHeaps(ctx.recorder);
			        m_queue.FlushDrawWithFrameAddr(ctx.recorder, ctx.frameSlot, &lightingAddr, m_constants.GetDeviceAddress(ctx.frameSlot), nullptr, 0, nullptr);
		        });

		// Resolve the preview HDR to LDR with the SAME tonemap operator + exposure as the
		// main view, so the thumbnail matches instead of showing raw (dark) HDR.
		graph
		        .AddFullscreenPass({
		                .name = "$CameraPreviewTonemap",
		                .color = m_colorLdr,
		                .extent = {kWidth, kHeight},
		                .loadOp = gpu::LoadOp::DontCare,
		        })
		        .ReadTexture(m_color)
		        .Execute(
		                [this, &bindless, &postProcess](PassContext& ctx)
		                {
			                gpu::CommandList& cmd = ctx.recorder;
			                bindless.CmdBindHeaps(cmd);
			                cmd.BindPipeline(postProcess.GetTonemapPipeline());
			                struct
			                {
				                std::uint32_t hdrSlot;
				                std::uint32_t mode;
				                float exposure;
				                std::uint32_t debugCompare;
				                std::uint32_t debugModeCount;
				                std::int32_t inspectX;
				                std::int32_t inspectY;
				                std::uint32_t screenWidth;
				                std::uint32_t screenHeight;
			                } push;
			                push.hdrSlot = m_colorBindlessSlot;
			                push.mode = static_cast<std::uint32_t>(postProcess.GetTonemapMode());
			                push.exposure = postProcess.GetExposure();
			                push.debugCompare = 0u;
			                push.debugModeCount = 0u;
			                push.inspectX = -1;
			                push.inspectY = -1;
			                push.screenWidth = kWidth;
			                push.screenHeight = kHeight;
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(3, 1, 0, 0);
		                });

		// A no-op read leaves the LDR resolve ShaderReadOnly so the UI panel can sample it.
		graph.AddPass("$CameraPreviewReady").ReadTexture(m_colorLdr).Execute([](PassContext&) {});
	}
} // namespace aether
