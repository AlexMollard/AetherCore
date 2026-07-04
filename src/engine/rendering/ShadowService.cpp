#include "rendering/ShadowService.hpp"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

#include "animation/AnimationDatabase.hpp"
#include "utils/Profiler.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuEnums.hpp"
#include "camera/CameraManager.hpp"
#include "passes/CullPass.hpp"
#include "rendering/WorldRenderer.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "scene/World.hpp"

namespace
{
	constexpr float kShadowFarCap = 320.0f;
	constexpr float kLightEyeBackoff = 120.0f;
	constexpr float kFarPlanePadding = 64.0f;
	constexpr float kCascadeRangePadding = 140.0f;

	// PSSM (practical split) blend factor.
	// 0.0 = uniform split in view-space distance, 1.0 = logarithmic split.
	// 0.65 gives a good balance: the first two cascades cover near-to-mid
	// range where perspective aliasing is most visible, while the last
	// cascade still reaches the shadow far cap.
	constexpr float kPssmLambda = 0.65f;

	// Minimum orthographic half-extent for each cascade.
	constexpr float kOrthoHalfMin = 20.0f;

	// Base orthographic half-extent = cascadeFar * kOrthoHalfViewRangeRatio.
	constexpr float kOrthoHalfViewRangeRatio = 0.60f;

	// Extra extent multiplier that forces cascade frustums to overlap. Beyond
	// hiding the boundary seam, overlap keeps a caster fully inside a cascade's
	// ortho box instead of being clipped at the edge ("half a shadow").
	constexpr float kCascadeOverlap = 1.30f;

	void DisableDirectionalShadows(aether::FrameConstants& fc)
	{
		fc.shadowParams.z = 0.0f;
	}
} // namespace

namespace aether
{
	void ShadowService::Initialize(VulkanContext& context, const Swapchain& swapchain, BindlessManager& bindless, const RenderQueueSharedPipelines& pipelines)
	{
		AE_PROFILE_ZONE();
		m_bindless = &bindless;

		for (auto& shadowConstants: m_shadowFrameConstants)
		{
			shadowConstants.Initialize();
		}

		static constexpr const char* kShadowDepthNames[kShadowCascadeCount] = {
		        "ShadowService.Depth_C0",
		        "ShadowService.Depth_C1",
		        "ShadowService.Depth_C2",
		};

		const gpu::Format depthFormat = swapchain.GetDepthFormat();
		m_shadowDepthFormat = depthFormat;
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			const gpu::TextureDesc desc{
			        .format = depthFormat,
			        .extent = {m_shadowMapExtents[cascade].width, m_shadowMapExtents[cascade].height},
			        .usage = gpu::ImageUsage::DepthStencilAttachment | gpu::ImageUsage::Sampled,
			        .aspect = gpu::ImageAspect::Depth,
			        .debugName = kShadowDepthNames[cascade],
			};
			m_shadowDepthHandle[cascade] = gpu::ResourceRegistry::CreateTexture(desc);
			if (!m_shadowDepthHandle[cascade].IsValid())
			{
				Throw(AetherError::Engine("ShadowService: CreateTexture failed for cascade " + std::to_string(cascade)));
			}
			m_shadowDepthImage[cascade] = gpu::ResourceRegistry::ResolveTextureImage(m_shadowDepthHandle[cascade]);
			m_shadowDepthView[cascade] = gpu::ResourceRegistry::ResolveTexture(m_shadowDepthHandle[cascade]).view;

			gpu::ResourceRegistry::EnsureBindlessSampled(m_shadowDepthHandle[cascade], gpu::ImageAspect::Depth, gpu::ImageLayout::ShaderReadOnly);
			m_shadowMapSlots[cascade] = gpu::ResourceRegistry::GetBindlessSampledSlot(m_shadowDepthHandle[cascade]);
			if (m_shadowMapSlots[cascade] == 0xFFFFFFFFu)
			{
				Throw(AetherError::Engine("ShadowService: bindless registration failed for cascade " + std::to_string(cascade)));
			}
		}

		// Single shadow queue with 3x output capacity for multi-frustum culling.
		m_shadowRenderQueue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 8192, .maxBatches = 1024, .maxAnimationDraws = UINT32_MAX, .outputDrawCapacity = 8192 * kCullMultiFrustumCount, .debugName = "DirectionalShadow"});
		AE_INFO(LogCategory::Render, "ShadowService RenderQueue initialized: maxSkinJoints={}, skinPaletteBuffer={}", m_shadowRenderQueue.GetMaxSkinJoints(), m_shadowRenderQueue.GetSkinPaletteBufferAddress());
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu); // Test: PoseInit + AnimSample

		RecreatePipeline(context.GetDevice().device, depthFormat);
	}

	void ShadowService::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.Shutdown();
		for (auto& shadowConstants: m_shadowFrameConstants)
		{
			shadowConstants.Shutdown();
		}
		m_shadowPipeline.Destroy();

		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			if (m_shadowDepthHandle[cascade].IsValid())
			{
				gpu::ResourceRegistry::Destroy(m_shadowDepthHandle[cascade]);
			}
			m_shadowDepthHandle[cascade] = {};
			m_shadowDepthImage[cascade] = nullptr;
			m_shadowDepthView[cascade] = nullptr;
			m_shadowMapSlots[cascade] = 0xFFFFFFFFu;
		}
		m_bindless = nullptr;
		m_shadowDepthFormat = gpu::Format::Undefined;
	}

	void ShadowService::RecreatePipeline(gpu::Device device, gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		m_shadowPipeline.Destroy();
		AE_EXPECT_OR_THROW(shadowPipeline,
		        GraphicsPipeline::Create(device,
		                {
		                        .shaderVfsPath = "shaders://shadow_depth.spv",
		                        .colorFormat = gpu::Format::Undefined,
		                        .depthFormat = depthFormat,
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .debugName = "ShadowService.Depth",
		                }));
		m_shadowPipeline = std::move(shadowPipeline);
	}

	void ShadowService::PrepareWriteSlot(const std::uint32_t drawSlot)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		m_shadowRenderQueue.Clear(drawSlot);
	}

	void ShadowService::PrepareQueues(const std::uint32_t drawSlot, World& world)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		if (!m_directionalShadowEnabled)
		{
			m_shadowRenderQueue.DiscardPending(drawSlot);
			return;
		}
		WorldRenderer::Flush(world, m_shadowRenderQueue);
	}

	void ShadowService::SetAnimationDatabase(const AnimationDatabase* animationDb)
	{
		m_shadowRenderQueue.SetAnimationDatabase(animationDb);
	}

	void ShadowService::RegisterPasses(RenderGraph& graph, const CullPass& cullPass)
	{
		SetupPassResources(graph);
		RegisterComputePasses(graph, cullPass);
		RegisterGraphicsPasses(graph);
	}

	void ShadowService::SetupPassResources(RenderGraph& graph)
	{
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			m_shadowDepth[cascade] = graph.RegisterImage(m_shadowDepthImage[cascade], m_shadowDepthView[cascade], gpu::ImageAspect::Depth);
		}
		(void) graph.GetBlackboard().DeclareGraphProduct<FrameTextureArrayProduct>(std::string{kFrameProductDirectionalShadows},
		        FrameTextureArrayProduct{
		                .images = std::vector<RGImage>{m_shadowDepth.begin(), m_shadowDepth.end()},
		                .bindlessSlots = std::vector<std::uint32_t>{m_shadowMapSlots.begin(), m_shadowMapSlots.end()},
		                .extents = std::vector<gpu::Extent2D>(m_shadowMapExtents.begin(), m_shadowMapExtents.end()),
		                .format = m_shadowDepthFormat,
		        });
	}

	void ShadowService::RegisterComputePasses(RenderGraph& graph, const CullPass& cullPass)
	{
		m_shadowDrawList = graph.CreatePreparedDrawList("DirectionalShadowDraws");
		auto pass = graph.AddComputePass("$CullDraws_Shadow");
		pass.DisableAsyncCompute()
		        .HasSideEffects("produces directional shadow RenderQueue prepared draw state")
		        .ProducesDrawList(m_shadowDrawList)
		        .ExecuteCompute(
		                [this, &cullPass](PassContext& ctx)
		                {
			                if (!IsDirectionalShadowEnabledForFrame(ctx.frameSlot))
			                {
				                m_shadowRenderQueue.DiscardPending(ctx.frameSlot);
				                return;
			                }
			                const auto frameIdx = ctx.frameSlot;
			                gpu::DeviceAddress cascadeAddrs[kCullMultiFrustumCount];
			                for (std::uint32_t c = 0; c < kCullMultiFrustumCount; ++c)
			                {
				                cascadeAddrs[c] = m_shadowFrameConstants[c].GetDeviceAddress(frameIdx);
			                }
			                m_shadowRenderQueue.SetMultiCullFrameAddrs(cascadeAddrs);
			                m_shadowRenderQueue.PrepareAndDispatch(ctx.recorder, cascadeAddrs[0], cullPass.GetMultiPipeline(), ctx.frameSlot);
		                })
		        .OnDebugDisabled([this](PassContext& ctx) { m_shadowRenderQueue.DiscardPending(ctx.frameSlot); });
	}

	void ShadowService::RegisterGraphicsPasses(RenderGraph& graph)
	{
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			const std::string idx = std::to_string(cascade);
			graph.AddPass("$DirectionalShadow_C" + idx)
			        .ConsumesDrawList(m_shadowDrawList)
			        .WriteDepth(m_shadowDepth[cascade], gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearDepthValue(1.0f))
			        .SetExtent(gpu::Extent2D{m_shadowMapExtents[cascade].width, m_shadowMapExtents[cascade].height})
			        .Execute(
			                [this, cascade](PassContext& ctx)
			                {
				                if (!IsDirectionalShadowEnabledForFrame(ctx.frameSlot))
				                {
					                return;
				                }
				                const std::uint32_t cascadeOffset = cascade * m_shadowRenderQueue.GetMaxDraws();
				                gpu::CommandList cmd = ctx.recorder.View();
				                m_shadowRenderQueue.FlushDraw(cmd, ctx.frameSlot, nullptr, &m_shadowPipeline, cascadeOffset);
			                });
		}

		graph.AddPass("$ShadowDepthTransition").ProducesProduct<FrameTextureArrayProduct>(kFrameProductDirectionalShadows).ReadTexture(m_shadowDepth[0]).ReadTexture(m_shadowDepth[1]).ReadTexture(m_shadowDepth[2]).Execute([](PassContext&) {});
	}

	void ShadowService::BuildFrameShadowData(const RenderFramePacket& packet, const std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc)
	{
		AE_PROFILE_ZONE();
		auto lightDir = glm::vec3(packet.sunDirectionIntensity);
		if (glm::length(lightDir) < 1e-4f)
		{
			lightDir = glm::vec3(0.5f, 0.8f, 0.2f);
		}
		lightDir = glm::normalize(lightDir);

		const bool directionalShadowFrameEnabled = packet.directionalShadowEnabled;
		m_directionalShadowFrameEnabled[frameIdx % kMaxFramesInFlight] = directionalShadowFrameEnabled;

		if (!directionalShadowFrameEnabled)
		{
			DisableDirectionalShadows(fc);
			return;
		}

		const Camera* mainCamForShadows = cameraManager.TryGetMainCamera();
		const float camNear = (mainCamForShadows != nullptr) ? mainCamForShadows->GetNearPlane() : 0.1f;
		const float camFar = (mainCamForShadows != nullptr) ? std::min(mainCamForShadows->GetFarPlane(), kShadowFarCap) : kShadowFarCap;
		const float viewRange = std::max(camFar - camNear, 1.0f);

		// Practical split (PSSM) — mixes logarithmic and uniform to balance
		// perspective aliasing against cascade count.
		float split0, split1, split2;
		{
			const float uni0 = camNear + viewRange * (1.0f / 3.0f);
			const float uni1 = camNear + viewRange * (2.0f / 3.0f);
			const float log0 = camNear * std::pow(camFar / camNear, 1.0f / 3.0f);
			const float log1 = camNear * std::pow(camFar / camNear, 2.0f / 3.0f);
			split0 = kPssmLambda * log0 + (1.0f - kPssmLambda) * uni0;
			split1 = kPssmLambda * log1 + (1.0f - kPssmLambda) * uni1;
			split2 = camFar;
		}

		fc.shadowCascadeSplits = glm::vec4(split0, split1, split2, 0.0f);
		fc.shadowParams = glm::vec4(0.0014f, 0.0030f, 1.0f, 2.0f);
		glm::vec3 camPos = packet.hasCameraData ? glm::vec3(packet.cameraWorldPos) : glm::vec3(0.0f);
		glm::vec3 camForward(0.0f, 0.0f, -1.0f);
		if (packet.hasCameraData)
		{
			const glm::mat4 invView = glm::inverse(packet.view);
			camForward = glm::normalize(glm::vec3(invView * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
		}

		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			const float cascadeNear = (cascade == 0u) ? camNear : fc.shadowCascadeSplits[cascade - 1u];
			const float cascadeFar = fc.shadowCascadeSplits[cascade];
			const float cascadeMid = 0.5f * (cascadeNear + cascadeFar);
			const float cascadeRange = std::max(cascadeFar - cascadeNear, 1.0f);

			glm::vec3 shadowCenter = camPos + camForward * cascadeMid;
			glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
			if (std::abs(glm::dot(up, lightDir)) > 0.95f)
			{
				up = glm::vec3(1.0f, 0.0f, 0.0f);
			}

			const float orthoHalf = std::max(kOrthoHalfMin, cascadeFar * kOrthoHalfViewRangeRatio) * kCascadeOverlap;
			glm::vec3 lightEye = shadowCenter + lightDir * (cascadeFar + kLightEyeBackoff);
			glm::mat4 lightView = glm::lookAt(lightEye, shadowCenter, up);

			const float texelSize = (2.0f * orthoHalf) / std::max(1.0f, static_cast<float>(m_shadowMapExtents[cascade].width));
			glm::vec3 centerLs = glm::vec3(lightView * glm::vec4(shadowCenter, 1.0f));
			centerLs.x = std::floor(centerLs.x / texelSize + 0.5f) * texelSize;
			centerLs.y = std::floor(centerLs.y / texelSize + 0.5f) * texelSize;

			const glm::mat4 invLightView = glm::inverse(lightView);
			shadowCenter = glm::vec3(invLightView * glm::vec4(centerLs, 1.0f));
			lightEye = shadowCenter + lightDir * (cascadeFar + kLightEyeBackoff);
			lightView = glm::lookAt(lightEye, shadowCenter, up);

			const float nearPlane = std::max(0.1f, cascadeNear * 0.5f);
			const float farPlane = std::max(nearPlane + kFarPlanePadding, cascadeFar + cascadeRange + kCascadeRangePadding);

			FrameConstants shadowFc{};
			shadowFc.view = lightView;
			shadowFc.proj = glm::ortho(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, nearPlane, farPlane);
			shadowFc.viewProj = shadowFc.proj * shadowFc.view;
			shadowFc.cameraWorldPos = glm::vec4(lightEye, 1.0f);
			shadowFc.materialBufferAddr = packet.materialBufferAddr;
			shadowFc.effectParamBufferAddr = packet.effectParamBufferAddr;
			shadowFc.sunDirectionIntensity = packet.sunDirectionIntensity;
			shadowFc.ambientColor = packet.ambientColor;
			shadowFc.sunColor = packet.sunColor;
			shadowFc.skyHorizonColor = packet.skyHorizonColor;
			shadowFc.skyZenithColor = packet.skyZenithColor;
			shadowFc.skyVoidColor = packet.skyVoidColor;
			shadowFc.RefreshDerived();

			m_shadowFrameConstants[cascade].Write(frameIdx, shadowFc);
			fc.shadowViewProjCascades[cascade] = shadowFc.viewProj;
		}
	}
} // namespace aether
