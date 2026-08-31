#include "rendering/ShadowService.hpp"

#include <math.h>

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>
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

	
	constexpr float kOrthoHalfMin = 20.0f;

	// Small outward margin on the fitted sphere so PCF taps near the cascade edge
	// still land on valid texels.
	constexpr float kCascadeOverlap = 1.04f;

	// Tangent of the sun's angular radius. The real sun subtends about half a
	// degree, which is what makes a shadow sharp at the caster's base and soft
	// metres away. Driving the penumbra from this rather than a filter-width
	// constant is why the softness stops depending on how far the camera is.
	constexpr float kSunTanAngularRadius = 0.00465f;

	// Tightest enclosing sphere of the view frustum slice between near and far.
	// Fitting a sphere rather than guessing a box is what makes coverage exact:
	// the previous ratio-of-far-plane guess was about 1.5x too small for a 60
	// degree camera, so the far third of every cascade fell outside its own map
	// and read as unshadowed - crisp at one distance, broken at the next. A
	// sphere is also rotation-invariant, so the fit does not breathe as the
	// camera turns.
	struct CascadeSphere
	{
		float centerDistance;
		float radius;
	};

	CascadeSphere FitCascadeSphere(const float nearZ, const float farZ, const float tanHalfX, const float tanHalfY)
	{
		const float a2 = tanHalfX * tanHalfX + tanHalfY * tanHalfY;
		const float center = (farZ + nearZ) * (a2 + 1.0f) * 0.5f;
		if (center >= farZ)
		{
			// Centre would sit past the far plane, so the far corners alone bound it.
			return CascadeSphere{.centerDistance = farZ, .radius = farZ * std::sqrt(a2)};
		}
		const float dz = nearZ - center;
		return CascadeSphere{.centerDistance = center, .radius = std::sqrt(nearZ * nearZ * a2 + dz * dz)};
	}

	// Shadow bias budget, in shadow-map texels of the cascade doing the lookup. The
	// offset only has to clear the filter that is actually in use, which contact
	// hardening keeps at a texel or two exactly where acne would otherwise show;
	// where the penumbra opens up the shadow is soft enough that acne cannot form.
	constexpr float kDepthBiasTexels = 1.0f;
	constexpr float kNormalOffsetTexels = 3.0f;


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

		const gpu::Format depthFormat = swapchain.GetDepthFormat();
		m_shadowDepthFormat = depthFormat;

		m_shadowRenderQueue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 8192, .maxBatches = 1024, .maxAnimationDraws = UINT32_MAX, .outputDrawCapacity = 8192 * kCullMultiFrustumCount, .debugName = "DirectionalShadow"});
		AE_INFO(LogCategory::Render, "ShadowService RenderQueue initialized: maxSkinJoints={}, skinPaletteBuffer={}", m_shadowRenderQueue.GetMaxSkinJoints(), m_shadowRenderQueue.GetSkinPaletteBufferAddress());
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu);

		RecreatePipeline(context.GetDevice().device, depthFormat);
	}

	void ShadowService::CreateShadowTargets()
	{
		AE_PROFILE_ZONE();
		if (m_shadowDepthFormat == gpu::Format::Undefined)
		{
			Throw(AetherError::Engine("ShadowService: CreateShadowTargets before Initialize"));
		}
		// No GPU work: the cascades are graph transients now, declared in SetupPassResources
		// on every graph build. This only records that the graph about to be built should
		// have shadow passes in it at all.
		m_shadowTargetsReady = true;
	}

	void ShadowService::DestroyShadowTargets()
	{
		AE_PROFILE_ZONE();
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			m_shadowMapSlots[cascade] = 0xFFFFFFFFu;
			m_shadowDepth[cascade] = {};
		}
		m_shadowTargetsReady = false;
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

		DestroyShadowTargets();
		m_bindless = nullptr;
		m_shadowDepthFormat = gpu::Format::Undefined;
	}

	void ShadowService::RecreatePipeline(gpu::Device device, gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		if (depthFormat != m_shadowDepthFormat)
		{
			// Existing cascade images carry the old format and can no longer be attached
			// alongside this pipeline. Drop them; the lazy-target pass recreates them in
			// the new format before the graph is rebuilt.
			DestroyShadowTargets();
			m_shadowDepthFormat = depthFormat;
		}
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

	bool ShadowService::PrepareQueues(const std::uint32_t drawSlot, World& world)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		if (!m_directionalShadowEnabled)
		{
			m_shadowRenderQueue.DiscardPending(drawSlot);
			return false;
		}
		// Depth only, so nothing blends and the transparent ordering cannot matter.
		WorldRenderer::Flush(world, m_shadowRenderQueue, glm::vec3(0.0f), /*shadowPass*/ true);
		const bool hasShadowCasters = !m_shadowRenderQueue.IsEmpty(drawSlot);
		if (!m_shadowTargetsReady)
		{
			// No cascade targets means no $CullDraws_Shadow pass to consume the slot, so
			// the commands are dropped here rather than left for a consumer that is not
			// in this frame's graph. The count above is still the honest content signal.
			m_shadowRenderQueue.DiscardPending(drawSlot);
		}
		return hasShadowCasters;
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
		// One cascade is written by its own $DirectionalShadow_Cn pass and read for the last
		// time by $EngineForward, all inside one frame. The bindless slots are reserved
		// against the graph so the lighting shaders can keep the numbers.
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			m_shadowDepth[cascade] = graph.CreateTransientDepth(m_shadowDepthFormat, m_shadowMapExtents[cascade], gpu::ImageUsage::Sampled);
			m_shadowMapSlots[cascade] = graph.EnsureBindlessSampled(m_shadowDepth[cascade]);
			if (m_shadowMapSlots[cascade] == 0xFFFFFFFFu)
			{
				Throw(AetherError::Engine("ShadowService: bindless registration failed for cascade " + std::to_string(cascade)));
			}
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

		// Without cascade targets there is nothing to sample, so the frame constants say
		// so and the shader's shadow term collapses to fully lit.
		const bool directionalShadowFrameEnabled = packet.directionalShadowEnabled && m_shadowTargetsReady;
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

		float split0 = NAN, split1 = NAN, split2 = NAN;
		{
			const float uni0 = camNear + viewRange * (1.0f / 3.0f);
			const float uni1 = camNear + viewRange * (2.0f / 3.0f);
			const float log0 = camNear * std::pow(camFar / camNear, 1.0f / 3.0f);
			const float log1 = camNear * std::pow(camFar / camNear, 2.0f / 3.0f);
			const float lambda = std::clamp(packet.shadowSplitLambda, 0.0f, 1.0f);
			split0 = lambda * log0 + (1.0f - lambda) * uni0;
			split1 = lambda * log1 + (1.0f - lambda) * uni1;
			split2 = camFar;
		}

		fc.shadowCascadeSplits = glm::vec4(split0, split1, split2, 0.0f);
		// x/y are bias budgets measured in shadow texels, not depth units: the per-cascade
		// conversion below turns them into a constant NDC bias and a world-space normal
		// offset using that cascade's own texel footprint and depth range. A depth-unit
		// constant cannot work here - one cascade's ortho spans a couple of hundred metres,
		// so 0.0014 of NDC was a third of a metre of peter-panning at the caster's feet.
		fc.shadowParams = glm::vec4(kDepthBiasTexels, kNormalOffsetTexels, 1.0f, packet.contactShadows ? 1.0f : 0.0f);
		// Half-angle tangents straight off the projection, so the fit tracks whatever
		// FOV and aspect the camera actually has instead of assuming one.
		const float tanHalfX = (std::abs(packet.proj[0][0]) > 1e-6f) ? (1.0f / std::abs(packet.proj[0][0])) : 1.0f;
		const float tanHalfY = (std::abs(packet.proj[1][1]) > 1e-6f) ? (1.0f / std::abs(packet.proj[1][1])) : 1.0f;

		const glm::vec3 camPos = packet.hasCameraData ? glm::vec3(packet.cameraWorldPos) : glm::vec3(0.0f);
		glm::vec3 camForward(0.0f, 0.0f, -1.0f);
		if (packet.hasCameraData)
		{
			const glm::mat4 invView = glm::inverse(packet.view);
			camForward = glm::normalize(glm::vec3(invView * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f)));
		}

		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			const float cascadeNear = (cascade == 0u) ? camNear : fc.shadowCascadeSplits[static_cast<glm::length_t>(cascade - 1u)];
			const float cascadeFar = fc.shadowCascadeSplits[static_cast<glm::length_t>(cascade)];
			const float cascadeRange = std::max(cascadeFar - cascadeNear, 1.0f);
			const CascadeSphere sphere = FitCascadeSphere(cascadeNear, cascadeFar, tanHalfX, tanHalfY);

			glm::vec3 shadowCenter = camPos + camForward * sphere.centerDistance;
			glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
			if (std::abs(glm::dot(up, lightDir)) > 0.95f)
			{
				up = glm::vec3(1.0f, 0.0f, 0.0f);
			}

			const float orthoHalf = std::max(kOrthoHalfMin, sphere.radius) * kCascadeOverlap;
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
			shadowFc.fogParams = packet.fogParams;
			shadowFc.skyParams = packet.skyParams;
			shadowFc.shadingParams = packet.shadingParams;
			shadowFc.RefreshDerived();

			// Constant bias stays at roughly one texel diagonal along the light: it only has
			// to cover depth quantisation on surfaces facing the light. Everything the slope
			// used to pay for is now the normal offset, which slides the lookup across the
			// surface instead of pushing it toward the light, so the silhouette stays put.
			const float depthRange = std::max(farPlane - nearPlane, 1e-3f);
			const auto cascadeIdx = static_cast<glm::length_t>(cascade);
			fc.shadowCascadeDepthBias[cascadeIdx] = (kDepthBiasTexels * texelSize * std::numbers::sqrt2_v<float>) / depthRange;
			fc.shadowCascadeNormalOffset[cascadeIdx] = kNormalOffsetTexels * texelSize;
			// A blocker one NDC unit in front of the receiver spans the cascade's whole
			// depth range, so its penumbra is that range times the sun's angular radius,
			// expressed as a fraction of the map's world width.
			fc.shadowCascadePenumbraScale[cascadeIdx] = (depthRange * kSunTanAngularRadius) / (2.0f * orthoHalf);

			m_shadowFrameConstants[cascade].Write(frameIdx, shadowFc);
			fc.shadowViewProjCascades[cascade] = shadowFc.viewProj;
		}
	}
} // namespace aether
