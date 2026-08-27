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
#include "rendering/Renderer2D.hpp"
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

		m_queue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 8192u, .debugName = "CameraPreview"});
		m_constants.Initialize();

		// The HDR colour and the depth are graph transients, declared in RegisterImages on
		// every graph build. Only the LDR image is created here, because ViewportPanel hands
		// its view to ImGui as a texture id and that descriptor names one specific image.
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

		m_initialized = m_colorLdrHandle.IsValid();
	}

	void CameraPreviewService::Shutdown()
	{
		m_queue.DiscardAllPending();
		m_queue.Shutdown();
		m_constants.Shutdown();
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
		m_colorLdrHandle = {};
		m_colorLdrView = nullptr;
		m_colorBindlessSlot = 0xFFFFFFFFu;
		m_initialized = false;
	}

	void CameraPreviewService::SetRequest(bool enabled, const glm::mat4& view, const glm::mat4& proj, glm::vec3 cameraPos, float nearPlane, bool drawSkybox)
	{
		{
			const std::lock_guard<std::mutex> lock(m_requestMutex);
			m_reqView = view;
			m_reqProj = proj;
			m_reqCameraPos = cameraPos;
			m_reqNearPlane = nearPlane;
		}
		m_drawSkybox.store(drawSkybox, std::memory_order_relaxed);
		m_enabled.store(enabled, std::memory_order_relaxed);
	}

	void CameraPreviewService::PrepareQueue(std::uint32_t drawSlot, World& world)
	{
		if (!m_initialized)
		{
			return;
		}
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
		FrameConstants fc = mainFc;
		float nearPlane = 0.1f;
		{
			const std::lock_guard<std::mutex> lock(m_requestMutex);
			fc.view = m_reqView;
			fc.proj = m_reqProj;
			fc.viewProj = m_reqProj * m_reqView;
			fc.cameraWorldPos = glm::vec4(m_reqCameraPos, 1.0f);
			nearPlane = m_reqNearPlane;
		}

		if (lighting != nullptr && m_lightView == kInvalidLightView)
		{
			m_lightView = lighting->RegisterView("CameraPreview");
			m_lighting = lighting;
		}
		const bool lit = lighting != nullptr && m_lightView != kInvalidLightView && m_enabled.load(std::memory_order_relaxed) && lighting->PrepareView(m_lightView, frameIdx, fc.view, fc.proj, nearPlane, {kWidth, kHeight}, fc);
		if (!lit)
		{
			fc.tiledLightGridInfo = glm::uvec4(0u);
			fc.tiledLightBufferOffsets = glm::uvec4(0u);
		}

		fc.RefreshDerived();
		m_constants.Write(frameIdx, fc);
	}

	void CameraPreviewService::RegisterImages(RenderGraph& graph)
	{
		if (!m_initialized)
		{
			return;
		}
		m_color = graph.CreateTransientColor(m_colorFormat, gpu::Extent2D{kWidth, kHeight}, gpu::ImageUsage::Sampled);
		m_colorBindlessSlot = graph.EnsureBindlessSampled(m_color);
		m_depth = graph.CreateTransientDepth(m_depthFormat, gpu::Extent2D{kWidth, kHeight});
		m_colorLdr = graph.RegisterImage(gpu::ResourceRegistry::ResolveTextureImage(m_colorLdrHandle), m_colorLdrView, gpu::ImageAspect::Color);
	}

	void CameraPreviewService::RegisterComputePasses(RenderGraph& graph, CullPass& cullPass)
	{
		if (!m_initialized)
		{
			return;
		}
		RegisterImages(graph);
		m_draws = graph.CreatePreparedDrawList("CameraPreviewDraws");

		graph.AddQueuePreparePass({
		                                  .name = "$CameraPreviewCull",
		                                  .produces = m_draws,
		                                  .sideEffectReason = "prepares camera-preview draw queue",
		                          })
		        .ExecuteCompute([this, &cullPass](PassContext& ctx) { m_queue.PrepareAndDispatch(ctx.recorder, m_constants.GetDeviceAddress(ctx.frameSlot), cullPass.GetSinglePipeline(), ctx.frameSlot); })
		        .OnDebugDisabled([this](PassContext& ctx) { m_queue.DiscardPending(ctx.frameSlot); });
	}

	void CameraPreviewService::RegisterGraphicsPasses(RenderGraph& graph,
	        LightingManager* lighting,
	        BindlessManager& bindless,
	        const PostProcessStack& postProcess,
	        gpu::PipelineView skyboxPipeline,
	        Renderer2D& renderer2D)
	{
		if (!m_initialized)
		{
			return;
		}

		graph.AddFullscreenPass({
		                                .name = "$CameraPreviewSkybox",
		                                .color = m_color,
		                                .extent = {kWidth, kHeight},
		                                .loadOp = gpu::LoadOp::Clear,
		                        })
		        .Execute(
		                [this, skyboxPipeline](PassContext& ctx)
		                {
			                if (!m_enabled.load(std::memory_order_relaxed) || !m_drawSkybox.load(std::memory_order_relaxed))
			                {
				                return;
			                }
			                gpu::CommandList& cmd = ctx.recorder;
			                cmd.BindPipeline(skyboxPipeline);
			                const gpu::DeviceAddress frameAddr = m_constants.GetDeviceAddress(ctx.frameSlot);
			                std::byte bytes[sizeof(gpu::DeviceAddress)];
			                std::memcpy(bytes, &frameAddr, sizeof(bytes));
			                cmd.PushDataRaw(0, std::span<const std::byte>(bytes, sizeof(bytes)));
			                cmd.Draw(3);
		                });

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
		        .colorLoadOp = gpu::LoadOp::Load,
		        .depthLoadOp = gpu::LoadOp::Clear,
		        .consumes = std::move(consumes),
		});
		// The shadow services only publish their products when their targets exist, and a
		// scene with nothing to shadow has none. Consuming an undeclared product would
		// leave the pass with a logical dependency no producer can satisfy.
		if (graph.GetBlackboard().TryGet<FrameTextureArrayProduct>(kFrameProductDirectionalShadows) != nullptr)
		{
			pass.ConsumeTextureProduct<FrameTextureArrayProduct>(kFrameProductDirectionalShadows, FrameResourceId::DirectionalShadowC0);
		}
		if (graph.GetBlackboard().TryGet<LocalShadowProduct>(kFrameProductLocalShadows) != nullptr)
		{
			pass.ConsumeTextureProduct<LocalShadowProduct>(kFrameProductLocalShadows, FrameResourceId::LocalShadowAtlas);
		}
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
				        return;
			        }
			        const DrawContracts::LightingAddresses lightingAddr = lighting != nullptr && m_lightView != kInvalidLightView ? lighting->GetLightingAddresses(m_lightView, ctx.frameSlot) : DrawContracts::LightingAddresses{};
			        bindless.CmdBindGlobalResources(ctx.recorder);
			        m_queue.FlushDrawWithFrameAddr(ctx.recorder, ctx.frameSlot, &lightingAddr, m_constants.GetDeviceAddress(ctx.frameSlot), nullptr, 0, nullptr);
		        });

		renderer2D.RegisterPass(graph, m_color, {kWidth, kHeight}, bindless, "$CameraPreviewRenderer2D", &m_constants, &m_enabled);

		graph.AddFullscreenPass({
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
			                bindless.CmdBindGlobalResources(cmd);
			                cmd.BindPipeline(postProcess.GetTonemapPipeline());
			                TonemapContracts::PushConstants push{};
			                push.hdrSlot = m_colorBindlessSlot;
			                push.mode = static_cast<std::uint32_t>(postProcess.GetTonemapMode());
			                push.exposure = postProcess.GetExposure();
			                push.debugCompare = 0u;
			                push.debugModeCount = 0u;
			                push.inspectX = -1;
			                push.inspectY = -1;
			                push.screenWidth = kWidth;
			                push.screenHeight = kHeight;
			                // The tonemap shader reads the full TonemapPush (48 bytes) incl. the
			                // background BDA; push the whole struct so it never dereferences an
			                // uninitialised device address. No camera background in this preview.
			                // Previews render their own small view with no lens of their own.
			                push.dofSlot = 0xFFFFFFFFu;
			                push.backgroundParamsAddr = 0u;
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(3, 1, 0, 0);
		                });

		graph.AddPass("$CameraPreviewReady").ReadTexture(m_colorLdr).Execute([](PassContext&) {});
	}
} // namespace aether
