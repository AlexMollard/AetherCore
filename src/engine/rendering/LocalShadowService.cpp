#include "rendering/LocalShadowService.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>

#include "camera/CameraManager.hpp"
#include "utils/Profiler.hpp"
#include "gpu/Bda.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "io/FileSystem.hpp"
#include "passes/CullPass.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/World.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

namespace
{
	constexpr std::uint32_t kPointLightFaceCount = 6u;
	constexpr std::uint32_t kMaxRenderedLocalShadowEntries = 48u;
	// EVSM exponential warp constant. Must match kEvsmExponent in
	constexpr float kEvsmExponent = 40.0f;
	constexpr std::uint32_t kPointShadowFaceRes = 384u;
	// which needs no separate coverage). Radial depth is face-invariant, so the
	constexpr float kPointLightFovDeg = 90.0f;
	constexpr float kSpotShadowFovPaddingRad = glm::radians(4.0f);

	// Fixed resolution for all spot shadows - avoids atlas layout shifts when
	// the light set changes between frames.
	constexpr std::uint32_t kSpotShadowRes = 512u;
	constexpr std::uint32_t kLargestShadowRes = std::max(kSpotShadowRes, kPointShadowFaceRes);

	// Worst-case height the shelf packer can consume, so the atlas is sized to the
	// entries that can actually land in it rather than to a round number.
	//
	// A shelf is only opened when no existing shelf of at least that height still
	// has room, so every shelf but the last of each height class is filled past
	// (atlasWidth - kLargestShadowRes). With two height classes in play (spot and
	// point-face) that leaves at most two partly-filled shelves.
	consteval std::uint32_t WorstCaseAtlasHeight()
	{
		constexpr std::uint32_t kTotalEntryWidth = kMaxRenderedLocalShadowEntries * kLargestShadowRes;
		constexpr std::uint32_t kFilledShelfWidth = aether::ShadowAtlasManager::kAtlasWidth - kLargestShadowRes;
		constexpr std::uint32_t kShelfCount = (kTotalEntryWidth / kFilledShelfWidth) + 2u;
		return kShelfCount * kLargestShadowRes;
	}

	static_assert(aether::ShadowAtlasManager::kAtlasWidth >= kLargestShadowRes, "Shadow atlas is narrower than a single shadow entry.");
	static_assert(WorstCaseAtlasHeight() <= aether::ShadowAtlasManager::kAtlasHeight,
	        "Shadow atlas is too small for kMaxRenderedLocalShadowEntries entries at this resolution - grow ShadowAtlasManager::kAtlasHeight (it costs 4x its own size in VSM blur scratch) or lower the entry cap.");

	struct PointShadowFace
	{
		glm::vec3 direction;
		glm::vec3 up;
	};

	constexpr std::array<PointShadowFace, kPointLightFaceCount> kPointShadowFaces{
	        PointShadowFace{.direction = glm::vec3(1.0f, 0.0f, 0.0f), .up = glm::vec3(0.0f, -1.0f, 0.0f)},
	        PointShadowFace{.direction = glm::vec3(-1.0f, 0.0f, 0.0f), .up = glm::vec3(0.0f, -1.0f, 0.0f)},
	        PointShadowFace{.direction = glm::vec3(0.0f, 1.0f, 0.0f), .up = glm::vec3(0.0f, 0.0f, 1.0f)},
	        PointShadowFace{.direction = glm::vec3(0.0f, -1.0f, 0.0f), .up = glm::vec3(0.0f, 0.0f, -1.0f)},
	        PointShadowFace{.direction = glm::vec3(0.0f, 0.0f, 1.0f), .up = glm::vec3(0.0f, -1.0f, 0.0f)},
	        PointShadowFace{.direction = glm::vec3(0.0f, 0.0f, -1.0f), .up = glm::vec3(0.0f, -1.0f, 0.0f)},
	};
} // namespace

namespace aether
{
	// Must match BlurPushConstants in vsm_blur.slang.
	struct BlurPushConstants
	{
		std::uint32_t atlasWidth;
		std::uint32_t atlasHeight;
		std::uint32_t blurOffsetX;
		std::uint32_t blurOffsetY;
		std::uint32_t isHorizontal;
		std::uint32_t blurWidth;
		std::uint32_t blurHeight;
		float _pad2;
		gpu::DeviceAddress srcAddr;
		gpu::DeviceAddress dstAddr;
	};

	static_assert(sizeof(BlurPushConstants) == 48, "BlurPushConstants must be 48 bytes");
	static_assert(offsetof(BlurPushConstants, atlasWidth) == 0, "BlurPushConstants atlasWidth offset mismatch");
	static_assert(offsetof(BlurPushConstants, blurOffsetX) == 8, "BlurPushConstants blurOffsetX offset mismatch");
	static_assert(offsetof(BlurPushConstants, isHorizontal) == 16, "BlurPushConstants isHorizontal offset mismatch");
	static_assert(offsetof(BlurPushConstants, blurWidth) == 20, "BlurPushConstants blurWidth offset mismatch");
	static_assert(offsetof(BlurPushConstants, blurHeight) == 24, "BlurPushConstants blurHeight offset mismatch");
	static_assert(offsetof(BlurPushConstants, _pad2) == 28, "BlurPushConstants _pad2 offset mismatch");
	static_assert(offsetof(BlurPushConstants, srcAddr) == 32, "BlurPushConstants srcAddr offset mismatch");
	static_assert(offsetof(BlurPushConstants, dstAddr) == 40, "BlurPushConstants dstAddr offset mismatch");

	void LocalShadowService::Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines)
	{
		AE_PROFILE_ZONE();
		auto* device = static_cast<gpu::Device>(context.GetDevice().device);
		(void) bindless;

		m_shadowRenderQueue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 4096, .maxBatches = 512, .maxAnimationDraws = 1024u, .debugName = "LocalShadow"});
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu);

		const gpu::Format depthFormat = swapchain.GetDepthFormat();
		AE_EXPECT_OR_THROW(pipeline,
		        GraphicsPipeline::Create(device,
		                {
		                        .shaderVfsPath = "shaders://local_shadow_depth.spv",
		                        .colorFormat = gpu::Format::R32G32Sfloat,
		                        .depthFormat = depthFormat,
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .cullMode = gpu::CullMode::Back,
		                        .debugName = "LocalShadow.Depth",
		                }));
		m_shadowPipeline = std::move(pipeline);

		m_perLightShadows.reserve(kMaxLocalShadows);
		m_lightShadowIndices.reserve(4096);

		constexpr gpu::BufferUsage kSsboBda = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress;

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			{
				const gpu::MappedBufferDesc desc{
				        .size = static_cast<gpu::DeviceSize>(kMaxLocalShadows) * sizeof(ShadowLightData),
				        .usage = kSsboBda,
				        .memoryUsage = gpu::MappedMemoryUsage::Auto,
				        .debugName = "LocalShadow.ShadowData",
				};
				m_shadowDataBuffer[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
				AE_ASSERT_ALWAYS(m_shadowDataBuffer[i].handle.IsValid(), "LocalShadowService: CreateMappedBuffer(ShadowData) failed");
				const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_shadowDataBuffer[i].handle);
				m_shadowDataBuffer[i].mapped = view.mappedPtr;
				m_shadowDataBuffer[i].address = view.deviceAddress;
			}
			{
				const gpu::MappedBufferDesc desc{
				        .size = static_cast<gpu::DeviceSize>(kMaxLocalShadows) * sizeof(FrameConstants),
				        .usage = kSsboBda,
				        .memoryUsage = gpu::MappedMemoryUsage::Auto,
				        .debugName = "LocalShadow.LightConstants",
				};
				m_lightConstantsBuffer[i].handle = gpu::ResourceRegistry::CreateMappedBuffer(desc);
				AE_ASSERT_ALWAYS(m_lightConstantsBuffer[i].handle.IsValid(), "LocalShadowService: CreateMappedBuffer(LightConstants) failed");
				const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(m_lightConstantsBuffer[i].handle);
				m_lightConstantsBuffer[i].mapped = view.mappedPtr;
				m_lightConstantsBuffer[i].address = view.deviceAddress;
			}
		}

		{
			m_blurPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(device,
			        gpu::ComputePipelineDesc{
			                .shaderVfsPath = "shaders://vsm_blur.spv",
			                .shaderEntry = "main",
			                .debugName = "VSMBlur",
			        });
			if (!m_blurPipelineHandle.IsValid())
			{
				AE_ASSERT_ALWAYS(false, "Failed to create VSM blur compute pipeline");
			}
		}

		m_atlasDepthFormat = depthFormat;
	}

	void LocalShadowService::CreateShadowTargets()
	{
		AE_PROFILE_ZONE();
		// No GPU work: the atlas is a graph transient, declared in SetupPassResources on every
		// graph build. The packer state lives on past a rebuild; only the image moves.
		m_atlasReady = true;
	}

	void LocalShadowService::DestroyShadowTargets()
	{
		AE_PROFILE_ZONE();
		m_atlasManager.Reset();
		m_atlasBindlessSlot = 0xFFFFFFFFu;
		m_atlasImage = {};
		m_atlasDepthImage = {};
		m_blurBufferRG = {};
		m_blurScratchBufferRG = {};
		m_perLightShadows.clear();
		m_atlasReady = false;
	}

	void LocalShadowService::Shutdown()
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.Shutdown();
		m_shadowPipeline.Destroy();
		for (auto& buf: m_shadowDataBuffer)
		{
			if (buf.handle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf.handle);
				buf.handle = {};
			}
			buf.mapped = nullptr;
			buf.address = 0;
		}
		for (auto& buf: m_lightConstantsBuffer)
		{
			if (buf.handle.IsValid())
			{
				gpu::ResourceRegistry::Destroy(buf.handle);
				buf.handle = {};
			}
			buf.mapped = nullptr;
			buf.address = 0;
		}
		DestroyShadowTargets();

		if (m_blurPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurPipelineHandle);
			m_blurPipelineHandle = {};
		}
	}

	bool LocalShadowService::PrepareQueues(const std::uint32_t drawSlot, World& world)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		if (!m_atlasReady)
		{
			// No atlas means no $CullLocalShadowDraws pass to consume the slot. Discard
			// first so Clear() below never waits on a consumer this frame's graph does
			// not contain, then still report what the world would have submitted.
			m_shadowRenderQueue.DiscardPending(drawSlot);
		}
		m_shadowRenderQueue.Clear(drawSlot);
		WorldRenderer::Flush(world, m_shadowRenderQueue, /*shadowPass*/ true);
		const bool hasShadowCasters = !m_shadowRenderQueue.IsEmpty(drawSlot);
		if (!m_atlasReady)
		{
			m_shadowRenderQueue.DiscardPending(drawSlot);
		}
		return hasShadowCasters;
	}

	void LocalShadowService::BuildFrameShadowData(const RenderFramePacket& packet, const std::uint32_t frameIdx, CameraManager& cameraManager, FrameConstants& fc)
	{
		AE_PROFILE_ZONE();
		(void) packet;

		if (!m_atlasReady)
		{
			// Every light reports "no shadow index" and the shader's local shadow term
			// collapses to fully lit, which is what a scene with nothing to shadow wants.
			m_perLightShadows.clear();
			m_lightShadowIndices.assign(packet.pointLights.size() + packet.spotLights.size(), glm::vec2(-1.0f, 1.0f));
			fc.shadowLightCount = 0;
			fc.shadowLightDataAddr = 0;
			(void) cameraManager;
			return;
		}

		m_atlasManager.Reset();
		m_perLightShadows.clear();

		const Camera* mainCam = cameraManager.TryGetMainCamera();
		const glm::vec3 camPos = (mainCam != nullptr) ? mainCam->GetPosition() : glm::vec3(0.0f);

		const auto pointCount = static_cast<std::uint32_t>(packet.pointLights.size());
		const auto spotCount = static_cast<std::uint32_t>(packet.spotLights.size());

		struct ShadowCandidate
		{
			glm::vec3 position;
			float radius;
			std::uint32_t lightIndex;
			std::uint32_t lightType;
			float distanceSq;
		};

		std::vector<ShadowCandidate> candidates;

		{
			const std::span<const Renderer::PointLight> ptLights = packet.pointLights;
			for (std::size_t i = 0; i < ptLights.size() && candidates.size() < kMaxLocalShadows; ++i)
			{
				if (!ptLights[i].castsShadow)
				{
					continue;
				}
				const glm::vec3 dPos = ptLights[i].position - camPos;
				const float dsq = glm::dot(dPos, dPos);
				candidates.push_back(ShadowCandidate{
				        .position = ptLights[i].position,
				        .radius = ptLights[i].radius,
				        .lightIndex = static_cast<std::uint32_t>(i),
				        .lightType = 0u,
				        .distanceSq = dsq,
				});
			}
		}

		{
			const std::span<const Renderer::SpotLight> spLights = packet.spotLights;
			for (std::size_t i = 0; i < spLights.size() && candidates.size() < kMaxLocalShadows; ++i)
			{
				if (!spLights[i].castsShadow)
				{
					continue;
				}
				const glm::vec3 dPos2 = spLights[i].position - camPos;
				const float dsq = glm::dot(dPos2, dPos2);
				candidates.push_back(ShadowCandidate{
				        .position = spLights[i].position,
				        .radius = spLights[i].radius,
				        .lightIndex = static_cast<std::uint32_t>(i),
				        .lightType = 1u,
				        .distanceSq = dsq,
				});
			}
		}

		std::ranges::sort(candidates,
		        [](const ShadowCandidate& a, const ShadowCandidate& b)
		        {
			        if (std::abs(a.distanceSq - b.distanceSq) > 1e-4f)
			        {
				        return a.distanceSq < b.distanceSq;
			        }
			        if (a.lightType != b.lightType)
			        {
				        return a.lightType < b.lightType;
			        }
			        return a.lightIndex < b.lightIndex;
		        });

		const std::uint32_t budget = std::min(static_cast<std::uint32_t>(candidates.size()), kMaxLocalShadows);

		const std::uint32_t totalLights = pointCount + spotCount;
		m_lightShadowIndices.assign(totalLights, glm::vec2(-1.0f, 1.0f));

		std::uint32_t shadowDataIdx = 0;

		for (std::uint32_t i = 0; i < budget; ++i)
		{
			const ShadowCandidate& c = candidates[i];

			if (c.lightType == 0u)
			{
				if (shadowDataIdx + kPointLightFaceCount > kMaxRenderedLocalShadowEntries)
				{
					continue;
				}

				const std::uint32_t faceRes = kPointShadowFaceRes;
				std::array<ShadowAtlasManager::Region, kPointLightFaceCount> regions{};
				bool allocatedAllFaces = true;
				for (std::uint32_t face = 0; face < kPointLightFaceCount; ++face)
				{
					regions[face] = m_atlasManager.Allocate(faceRes, faceRes);
					allocatedAllFaces = allocatedAllFaces && regions[face].IsValid();
				}
				if (!allocatedAllFaces)
				{
					break;
				}

				for (std::uint32_t face = 0; face < kPointLightFaceCount; ++face)
				{
					const PointShadowFace& faceDesc = kPointShadowFaces[face];
					const glm::mat4 lightView = glm::lookAt(c.position, c.position + faceDesc.direction, faceDesc.up);
					const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(glm::radians(kPointLightFovDeg), 1.0f, 1.0f, 0.1f, c.radius);
					m_perLightShadows.push_back(PerLightShadow{
					        .viewProj = lightProj * lightView,
					        .region = regions[face],
					        .depthBias = 0.01f,
					        .normalBias = 0.03f,
					        .lightType = 1u,
					        .lightPosRange = glm::vec4(c.position, c.radius),
					});
				}

				m_lightShadowIndices[c.lightIndex] = glm::vec2(static_cast<float>(shadowDataIdx), 1.0f);
				shadowDataIdx += kPointLightFaceCount;
			}
			else
			{
				if (shadowDataIdx + 1u > kMaxRenderedLocalShadowEntries)
				{
					break;
				}

				const ShadowAtlasManager::Region r = m_atlasManager.Allocate(kSpotShadowRes, kSpotShadowRes);
				if (!r.IsValid())
				{
					break;
				}

				const std::span<const Renderer::SpotLight> spLights = packet.spotLights;
				const Renderer::SpotLight& src = spLights[c.lightIndex];
				const glm::vec3 lightDir = glm::normalize(src.direction);
				const glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
				const glm::mat4 lightView = glm::lookAt(src.position, src.position + lightDir, up);
				const float fov = std::min(2.0f * (src.outerAngleRad + kSpotShadowFovPaddingRad), glm::radians(175.0f));
				const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(fov, 1.0f, 1.0f, 0.1f, src.radius);

				m_perLightShadows.push_back(PerLightShadow{
				        .viewProj = lightProj * lightView,
				        .region = r,
				        .depthBias = 0.01f,
				        .normalBias = 0.03f,
				        .lightType = 0u,
				        .lightPosRange = glm::vec4(src.position, src.radius),
				});

				m_lightShadowIndices[pointCount + c.lightIndex] = glm::vec2(static_cast<float>(shadowDataIdx), 1.0f);
				shadowDataIdx += 1u;
			}
		}

		const auto shadowCount = static_cast<std::uint32_t>(m_perLightShadows.size());
		const std::uint32_t bufSlot = frameIdx % kMaxFramesInFlight;
		auto* mapped = static_cast<ShadowLightData*>(m_shadowDataBuffer[bufSlot].mapped);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			const PerLightShadow& pls = m_perLightShadows[i];
			mapped[i].viewProj = pls.viewProj;
			mapped[i].atlasRegion = glm::vec4(static_cast<float>(pls.region.x) / static_cast<float>(ShadowAtlasManager::kAtlasWidth),
			        static_cast<float>(pls.region.y) / static_cast<float>(ShadowAtlasManager::kAtlasHeight),
			        static_cast<float>(pls.region.width) / static_cast<float>(ShadowAtlasManager::kAtlasWidth),
			        static_cast<float>(pls.region.height) / static_cast<float>(ShadowAtlasManager::kAtlasHeight));
			mapped[i].depthBias = pls.depthBias;
			mapped[i].normalBias = pls.normalBias;
			mapped[i].lightType = pls.lightType;
			mapped[i].lightPosRange = pls.lightPosRange;
		}
		gpu::ResourceRegistry::FlushMappedBuffer(m_shadowDataBuffer[bufSlot].handle, 0, static_cast<gpu::DeviceSize>(shadowCount) * sizeof(ShadowLightData));

		auto* lightFc = static_cast<FrameConstants*>(m_lightConstantsBuffer[bufSlot].mapped);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			lightFc[i].viewProj = m_perLightShadows[i].viewProj;
			lightFc[i].cameraWorldPos = m_perLightShadows[i].lightPosRange;
			lightFc[i].RefreshDerived();
		}
		gpu::ResourceRegistry::FlushMappedBuffer(m_lightConstantsBuffer[bufSlot].handle, 0, static_cast<gpu::DeviceSize>(shadowCount) * sizeof(FrameConstants));

		fc.shadowLightCount = shadowCount;
		fc.shadowLightDataAddr = m_shadowDataBuffer[bufSlot].address;
	}

	void LocalShadowService::RegisterPasses(RenderGraph& graph, CullPass& cullPass)
	{
		SetupPassResources(graph);
		RegisterComputePasses(graph, cullPass);
		RegisterGraphicsPasses(graph);
	}

	void LocalShadowService::SetupPassResources(RenderGraph& graph)
	{
		// Written by $LocalShadowAtlasRender, blurred through the buffer chain and read for the
		// last time by $EngineForward - 128 MiB that never outlives the frame.
		m_atlasImage = graph.CreateTransientColor(ShadowAtlasManager::kAtlasFormat,
		        gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
		        gpu::ImageUsage::TransferSrc | gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::Storage);
		m_atlasBindlessSlot = graph.EnsureBindlessSampled(m_atlasImage);
		if (m_atlasBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("LocalShadowService: shadow atlas bindless registration failed"));
		}

		constexpr gpu::DeviceSize kBlurBufSize = static_cast<gpu::DeviceSize>(ShadowAtlasManager::kAtlasWidth) * ShadowAtlasManager::kAtlasHeight * sizeof(float) * 2u;
		constexpr gpu::BufferUsage kBlurBufUsage = gpu::BufferUsage::Storage | gpu::BufferUsage::TransferSrc | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
		m_blurBufferRG = graph.CreateTransientBuffer(kBlurBufSize, kBlurBufUsage);
		m_blurScratchBufferRG = graph.CreateTransientBuffer(kBlurBufSize, kBlurBufUsage);

		m_atlasDepthImage = graph.CreateTransientDepth(m_atlasDepthFormat, gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight});
		(void) graph.GetBlackboard().DeclareGraphProduct<LocalShadowProduct>(std::string{kFrameProductLocalShadows},
		        LocalShadowProduct{
		                .atlasImage = m_atlasImage,
		                .atlasDepthImage = m_atlasDepthImage,
		                .atlasBindlessSlot = m_atlasBindlessSlot,
		                .atlasExtent = gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
		                .atlasFormat = gpu::Format::R32G32Sfloat,
		        });
	}

	void LocalShadowService::RegisterComputePasses(RenderGraph& graph, CullPass& cullPass)
	{
		m_shadowDrawList = graph.CreatePreparedDrawList("LocalShadowDraws");
		auto pass = graph.AddComputePass("$CullLocalShadowDraws");
		pass.DisableAsyncCompute()
		        .HasSideEffects("produces local shadow RenderQueue prepared draw state")
		        .ProducesDrawList(m_shadowDrawList)
		        .ExecuteCompute(
		                [this, &cullPass](PassContext& ctx)
		                {
			                // it cliffs into a render-thread wedge the moment a shadow-caster is
			                m_shadowRenderQueue.SetDebugForceVisible(false);
			                m_shadowRenderQueue.PrepareAndDispatch(ctx.recorder, ctx.frameConstantsAddr, cullPass.GetSinglePipeline(), ctx.frameSlot);
		                })
		        .OnDebugDisabled([this](PassContext& ctx) { m_shadowRenderQueue.DiscardPending(ctx.frameSlot); });
	}

	void LocalShadowService::RegisterGraphicsPasses(RenderGraph& graph)
	{
		graph.AddPass("$LocalShadowAtlasRender")
		        .ConsumesDrawList(m_shadowDrawList)
		        .WriteColor(m_atlasImage, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(std::exp(kEvsmExponent), std::exp(2.0f * kEvsmExponent), 0.0f, 0.0f))
		        .WriteDepth(m_atlasDepthImage, gpu::LoadOp::Clear, gpu::StoreOp::DontCare, ClearDepthValue(1.0f))
		        .SetExtent(gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight})
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (m_perLightShadows.empty())
			                {
				                return;
			                }

			                gpu::CommandList cmd = ctx.recorder.View();
			                for (std::uint32_t li = 0; li < static_cast<std::uint32_t>(m_perLightShadows.size()); ++li)
			                {
				                const PerLightShadow& pls = m_perLightShadows[li];

				                const gpu::Viewport vp{
				                        .x = static_cast<float>(pls.region.x),
				                        .y = static_cast<float>(pls.region.y),
				                        .width = static_cast<float>(pls.region.width),
				                        .height = static_cast<float>(pls.region.height),
				                        .minDepth = 0.0f,
				                        .maxDepth = 1.0f,
				                };
				                cmd.SetViewport(vp);

				                const gpu::Rect2D scissor{
				                        .x = static_cast<std::int32_t>(pls.region.x),
				                        .y = static_cast<std::int32_t>(pls.region.y),
				                        .width = static_cast<std::uint32_t>(pls.region.width),
				                        .height = static_cast<std::uint32_t>(pls.region.height),
				                };
				                cmd.SetScissor(scissor);

				                const gpu::DeviceAddress lightFcAddr = m_lightConstantsBuffer[ctx.frameSlot].address + static_cast<gpu::DeviceSize>(li) * sizeof(FrameConstants);
				                m_shadowRenderQueue.FlushDrawWithFrameAddr(cmd, ctx.frameSlot, nullptr, lightFcAddr, &m_shadowPipeline);
			                }
		                });

		graph.AddComputePass("$VSMCopyToBuffer")
		        .ReadImageTransfer(m_atlasImage)
		        .WriteBufferTransfer(m_blurBufferRG)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return;
			                }

			                auto* const atlasImage = ctx.graph.ResolveImage(m_atlasImage);
			                auto* const blurVkBuf = ctx.graph.ResolveBuffer(m_blurBufferRG);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.CopyImageToBuffer(atlasImage, blurVkBuf, gpu::ImageLayout::TransferSrc, gpu::ImageAspect::Color, bounds.width, bounds.height, 0, static_cast<std::int32_t>(bounds.x), static_cast<std::int32_t>(bounds.y));
		                });

		graph.AddComputeBufferPass({
		                                   .name = "$VSMBlurH",
		                                   .reads = {m_blurBufferRG},
		                                   .writes = {m_blurScratchBufferRG},
		                           })
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return;
			                }

			                // Pooled buffers move whenever the graph is rebuilt, so the
			                // addresses are read now rather than cached at construction.
			                const gpu::DeviceAddress srcAddr = ctx.graph.GetBufferAddress(m_blurBufferRG);
			                const gpu::DeviceAddress dstAddr = ctx.graph.GetBufferAddress(m_blurScratchBufferRG);

			                const auto blurPipeline = gpu::ResourceRegistry::ResolvePipeline(m_blurPipelineHandle);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(blurPipeline.state);
			                for (const PerLightShadow& pls: m_perLightShadows)
			                {
				                if (!pls.region.IsValid())
				                {
					                continue;
				                }

				                const BlurPushConstants hPc{
				                        .atlasWidth = bounds.width,
				                        .atlasHeight = bounds.height,
				                        .blurOffsetX = pls.region.x - bounds.x,
				                        .blurOffsetY = pls.region.y - bounds.y,
				                        .isHorizontal = 1u,
				                        .blurWidth = pls.region.width,
				                        .blurHeight = pls.region.height,
				                        ._pad2 = 0.0f,
				                        .srcAddr = srcAddr,
				                        .dstAddr = dstAddr,
				                };
				                cmd.PushDataRaw(0, std::as_bytes(std::span{&hPc, 1}));
				                cmd.Dispatch((pls.region.width + 15u) / 16u, (pls.region.height + 15u) / 16u, 1u);
			                }
		                });

		graph.AddComputeBufferPass({
		                                   .name = "$VSMBlurV",
		                                   .reads = {m_blurScratchBufferRG},
		                                   .writes = {m_blurBufferRG},
		                           })
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return;
			                }

			                const gpu::DeviceAddress srcAddr = ctx.graph.GetBufferAddress(m_blurScratchBufferRG);
			                const gpu::DeviceAddress dstAddr = ctx.graph.GetBufferAddress(m_blurBufferRG);

			                const auto blurPipeline = gpu::ResourceRegistry::ResolvePipeline(m_blurPipelineHandle);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(blurPipeline.state);
			                for (const PerLightShadow& pls: m_perLightShadows)
			                {
				                if (!pls.region.IsValid())
				                {
					                continue;
				                }

				                const BlurPushConstants vPc{
				                        .atlasWidth = bounds.width,
				                        .atlasHeight = bounds.height,
				                        .blurOffsetX = pls.region.x - bounds.x,
				                        .blurOffsetY = pls.region.y - bounds.y,
				                        .isHorizontal = 0u,
				                        .blurWidth = pls.region.width,
				                        .blurHeight = pls.region.height,
				                        ._pad2 = 0.0f,
				                        .srcAddr = srcAddr,
				                        .dstAddr = dstAddr,
				                };
				                cmd.PushDataRaw(0, std::as_bytes(std::span{&vPc, 1}));
				                cmd.Dispatch((pls.region.width + 15u) / 16u, (pls.region.height + 15u) / 16u, 1u);
			                }
		                });

		graph.AddComputePass("$VSMCopyToAtlas")
		        .ProducesProduct<LocalShadowProduct>(kFrameProductLocalShadows)
		        .ReadBufferTransfer(m_blurBufferRG)
		        .WriteImageTransfer(m_atlasImage)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return;
			                }

			                auto* const atlasImage = ctx.graph.ResolveImage(m_atlasImage);
			                auto* const blurVkBuf = ctx.graph.ResolveBuffer(m_blurBufferRG);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.CopyBufferToImage(blurVkBuf, atlasImage, gpu::ImageLayout::TransferDst, gpu::ImageAspect::Color, bounds.width, bounds.height, 0, static_cast<std::int32_t>(bounds.x), static_cast<std::int32_t>(bounds.y));
		                });
	}
} // namespace aether
