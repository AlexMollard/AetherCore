#include "passes/ShadowService.hpp"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

#include "animation/AnimationDatabase.hpp"
#include "material/BindlessManager.hpp"
#include "camera/CameraManager.hpp"
#include "passes/CullPass.hpp"
#include "scene/Scene.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"
#include "scene/World.hpp"

namespace aether
{
	void ShadowService::Initialize(VulkanContext& context, const Swapchain& swapchain)
	{
		for (auto& shadowConstants: m_shadowFrameConstants)
		{
			shadowConstants.Initialize(context);
		}
		for (auto& shadowQueue: m_shadowRenderQueues)
		{
			shadowQueue.Initialize(context.GetDevice().device, context.GetAllocator());
		}
		for (auto& voxelQueue: m_voxelShadowRenderQueues)
		{
			// Voxel chunks: one batch per unique mesh, no skinning ever.
			// 512 draws / 512 batches covers worlds up to ~170 chunks per cascade;
			// maxAnimationDraws=0 skips all animation/skin buffer allocation entirely.
			voxelQueue.Initialize(context.GetDevice().device, context.GetAllocator(), 512, 512, 0u);
		}

		RecreatePipeline(context.GetDevice().device, swapchain.GetDepthFormat());
	}

	void ShadowService::Shutdown(const VkDevice device)
	{
		for (auto& shadowQueue: m_shadowRenderQueues)
		{
			shadowQueue.Shutdown();
		}
		for (auto& voxelQueue: m_voxelShadowRenderQueues)
		{
			voxelQueue.Shutdown();
		}
		for (auto& shadowConstants: m_shadowFrameConstants)
		{
			shadowConstants.Shutdown();
		}
		m_shadowPipeline.Destroy();
		m_voxelShadowPipeline.Destroy();
		(void) device;
	}

	void ShadowService::RecreatePipeline(VkDevice device, VkFormat depthFormat)
	{
		m_shadowPipeline.Destroy();
		m_shadowPipeline = GraphicsPipeline::Create(device,
		        {
		                .shaderVfsPath = "shaders://shadow_depth.slang.spv",
		                .colorFormat = VK_FORMAT_UNDEFINED,
		                .depthFormat = depthFormat,
		                .depthTestEnable = true,
		                .depthWriteEnable = true,
		                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		        });
		m_voxelShadowPipeline.Destroy();
		m_voxelShadowPipeline = GraphicsPipeline::Create(device,
		        {
		                .shaderVfsPath = "shaders://voxel_shadow_depth.slang.spv",
		                .colorFormat = VK_FORMAT_UNDEFINED,
		                .depthFormat = depthFormat,
		                .depthTestEnable = true,
		                .depthWriteEnable = true,
		                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		        });
	}

	void ShadowService::PrepareWriteSlot(const std::uint32_t drawSlot)
	{
		for (auto& shadowQueue: m_shadowRenderQueues)
		{
			shadowQueue.SetWriteSlot(drawSlot);
			shadowQueue.Clear(drawSlot);
		}
		for (auto& voxelQueue: m_voxelShadowRenderQueues)
		{
			voxelQueue.SetWriteSlot(drawSlot);
			voxelQueue.Clear(drawSlot);
		}
	}

	void ShadowService::PrepareQueues(const std::uint32_t drawSlot, Scene& scene, World& world)
	{
		for (auto& shadowQueue: m_shadowRenderQueues)
		{
			shadowQueue.SetWriteSlot(drawSlot);
			scene.FlushToQueue(shadowQueue);
			world.FlushToQueue(shadowQueue);
		}
	}

	void ShadowService::SetAnimationDatabase(const AnimationDatabase* animationDb)
	{
		for (auto& shadowQueue: m_shadowRenderQueues)
		{
			shadowQueue.SetAnimationDatabase(animationDb);
		}
	}

	void ShadowService::SubmitShadowCaster(const DrawCommand& cmd)
	{
		for (auto& voxelQueue: m_voxelShadowRenderQueues)
		{
			voxelQueue.Submit(cmd);
		}
	}

	void ShadowService::RegisterPasses(RenderGraph& graph, BindlessManager& bindlessManager, VkDevice device, const CullPass& cullPass, VkFormat depthFormat)
	{
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			m_shadowDepth[cascade] = graph.CreateTransientDepth(depthFormat, m_shadowMapExtents[cascade], VK_IMAGE_USAGE_SAMPLED_BIT);
			m_shadowMapSlots[cascade] = graph.EnsureBindlessSampled(m_shadowDepth[cascade], bindlessManager, device);

			const std::string idx = std::to_string(cascade);
			graph.AddComputePass("$CullDraws_Shadow_C" + idx)
			        .ExecuteCompute(
			                [this, cascade, &cullPass](PassContext& ctx)
			                {
				                const auto frameIdx = static_cast<std::uint32_t>(ctx.frameIndex % Swapchain::kMaxFramesInFlight);
				                const VkDeviceAddress shadowFrameAddr = m_shadowFrameConstants[cascade].GetDeviceAddress(frameIdx);
				                m_shadowRenderQueues[cascade].PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), shadowFrameAddr, cullPass.GetPipeline(), cullPass.GetPipelineLayout(), ctx.frameIndex);
				                m_voxelShadowRenderQueues[cascade].PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), shadowFrameAddr, cullPass.GetPipeline(), cullPass.GetPipelineLayout(), ctx.frameIndex);
			                });

			graph.AddPass("$DirectionalShadow_C" + idx)
			        .WriteDepth(m_shadowDepth[cascade], VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearDepthValue(1.0f))
			        .SetExtent(m_shadowMapExtents[cascade])
			        .Execute(
			                [this, cascade](PassContext& ctx)
			                {
				                m_shadowRenderQueues[cascade].FlushDraw(ctx.recorder, VK_NULL_HANDLE, VK_NULL_HANDLE, &m_shadowPipeline);
				                m_shadowRenderQueues[cascade].Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
				                m_voxelShadowRenderQueues[cascade].FlushDraw(ctx.recorder, VK_NULL_HANDLE, VK_NULL_HANDLE, &m_voxelShadowPipeline);
				                m_voxelShadowRenderQueues[cascade].Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
			                });
		}
	}

	void ShadowService::BuildFrameShadowData(const RenderFramePacket& packet, const std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc)
	{
		glm::vec3 lightDir = glm::vec3(packet.sunDirectionIntensity);
		if (glm::length(lightDir) < 1e-4f)
		{
			lightDir = glm::vec3(0.5f, 0.8f, 0.2f);
		}
		lightDir = glm::normalize(lightDir);

		const Camera* mainCamForShadows = cameraManager.TryGetMainCamera();
		const float camNear = (mainCamForShadows != nullptr) ? mainCamForShadows->GetNearPlane() : 0.1f;
		const float camFar = (mainCamForShadows != nullptr) ? std::min(mainCamForShadows->GetFarPlane(), 320.0f) : 320.0f;
		const float viewRange = std::max(camFar - camNear, 1.0f);
		const float split0 = camNear + viewRange * 0.08f;
		const float split1 = camNear + viewRange * 0.28f;
		const float split2 = camNear + viewRange * 0.72f;

		fc.shadowCascadeSplits = glm::vec4(split0, split1, split2, 0.0f);
		fc.shadowParams = glm::vec4(0.0007f, 0.0012f, 1.0f, 1.5f);
		for (std::uint32_t cascade = 0; cascade < kShadowCascadeCount; ++cascade)
		{
			fc.shadowViewProjCascades[cascade] = glm::mat4(1.0f);
			fc.shadowCascadeInfo[cascade] = glm::uvec4(0xFFFFFFFFu, 0u, 0u, 0u);
		}

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

			const float orthoHalf = std::max(20.0f, cascadeFar * 0.60f);
			glm::vec3 lightEye = shadowCenter + lightDir * (cascadeFar + 120.0f);
			glm::mat4 lightView = glm::lookAt(lightEye, shadowCenter, up);

			// Texel-snap to suppress cascade shimmer while moving the camera.
			const float texelSize = (2.0f * orthoHalf) / std::max(1.0f, static_cast<float>(m_shadowMapExtents[cascade].width));
			glm::vec3 centerLs = glm::vec3(lightView * glm::vec4(shadowCenter, 1.0f));
			centerLs.x = std::floor(centerLs.x / texelSize + 0.5f) * texelSize;
			centerLs.y = std::floor(centerLs.y / texelSize + 0.5f) * texelSize;

			const glm::mat4 invLightView = glm::inverse(lightView);
			shadowCenter = glm::vec3(invLightView * glm::vec4(centerLs, 1.0f));
			lightEye = shadowCenter + lightDir * (cascadeFar + 120.0f);
			lightView = glm::lookAt(lightEye, shadowCenter, up);

			const float nearPlane = std::max(0.1f, cascadeNear * 0.5f);
			const float farPlane = std::max(nearPlane + 64.0f, cascadeFar + cascadeRange + 140.0f);

			FrameConstants shadowFc{};
			shadowFc.view = lightView;
			shadowFc.proj = glm::ortho(-orthoHalf, orthoHalf, -orthoHalf, orthoHalf, nearPlane, farPlane);
			shadowFc.viewProj = shadowFc.proj * shadowFc.view;
			shadowFc.cameraWorldPos = glm::vec4(lightEye, 1.0f);
			shadowFc.materialBufferAddr = packet.materialBufferAddr;
			shadowFc.sunDirectionIntensity = packet.sunDirectionIntensity;
			shadowFc.ambientColor = packet.ambientColor;
			shadowFc.sunColor = packet.sunColor;
			shadowFc.skyHorizonColor = packet.skyHorizonColor;
			shadowFc.skyZenithColor = packet.skyZenithColor;
			shadowFc.skyVoidColor = packet.skyVoidColor;

			m_shadowFrameConstants[cascade].Write(frameIdx, shadowFc);
			fc.shadowViewProjCascades[cascade] = shadowFc.viewProj;
			fc.shadowCascadeInfo[cascade] = glm::uvec4(m_shadowMapSlots[cascade], m_shadowMapExtents[cascade].width, m_shadowMapExtents[cascade].height, 0u);
		}
	}
} // namespace aether
