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
	constexpr std::uint32_t kMaxRenderedLocalShadowEntries = 24u;
	constexpr std::uint32_t kPointShadowFaceRes = 384u;
	constexpr float kPointLightFovDeg = 100.0f;
	constexpr float kSpotShadowFovPaddingRad = glm::radians(4.0f);

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
		float _pad0;
		float _pad1;
		float _pad2;
		gpu::DeviceAddress srcAddr;
		gpu::DeviceAddress dstAddr;
	};

	static_assert(sizeof(BlurPushConstants) == 48, "BlurPushConstants must be 48 bytes");
	static_assert(offsetof(BlurPushConstants, atlasWidth) == 0, "BlurPushConstants atlasWidth offset mismatch");
	static_assert(offsetof(BlurPushConstants, blurOffsetX) == 8, "BlurPushConstants blurOffsetX offset mismatch");
	static_assert(offsetof(BlurPushConstants, isHorizontal) == 16, "BlurPushConstants isHorizontal offset mismatch");
	static_assert(offsetof(BlurPushConstants, _pad0) == 20, "BlurPushConstants _pad0 offset mismatch");
	static_assert(offsetof(BlurPushConstants, _pad1) == 24, "BlurPushConstants _pad1 offset mismatch");
	static_assert(offsetof(BlurPushConstants, _pad2) == 28, "BlurPushConstants _pad2 offset mismatch");
	static_assert(offsetof(BlurPushConstants, srcAddr) == 32, "BlurPushConstants srcAddr offset mismatch");
	static_assert(offsetof(BlurPushConstants, dstAddr) == 40, "BlurPushConstants dstAddr offset mismatch");

	void LocalShadowService::Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines)
	{
		AE_PROFILE_ZONE();
		auto device = static_cast<gpu::Device>(context.GetDevice().device);

		m_atlasManager.Initialize(bindless);
		m_atlasBindlessSlot = m_atlasManager.GetBindlessSlot();

		m_shadowRenderQueue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 4096, .maxBatches = 512, .maxAnimationDraws = 1024u, .debugName = "LocalShadow"});
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu); // Test: PoseInit + AnimSample

		// Create the shadow depth pipeline (reads VP from per-light FrameConstants via BDA).
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
		                        .debugName = "LocalShadow.Depth",
		                }));
		m_shadowPipeline = std::move(pipeline);

		m_perLightShadows.reserve(kMaxLocalShadows);
		m_lightShadowIndices.reserve(4096);

		// Allocate per-frame GPU buffers for per-light shadow data and light constants (double-buffered).
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

		// -- Create VSM blur buffer (BDA) -----------------------------------
		{
			constexpr gpu::DeviceSize kBlurBufSize = static_cast<gpu::DeviceSize>(ShadowAtlasManager::kAtlasWidth) * ShadowAtlasManager::kAtlasHeight * sizeof(float) * 2u;
			constexpr gpu::BufferUsage kBlurBufUsage = gpu::BufferUsage::Storage | gpu::BufferUsage::TransferSrc | gpu::BufferUsage::TransferDst | gpu::BufferUsage::ShaderDeviceAddress;
			m_blurBuffer = gpu::ResourceRegistry::CreateBuffer({
			        .size = kBlurBufSize,
			        .usage = kBlurBufUsage,
			        .debugName = "ShadowBlurBuffer",
			});
			AE_ASSERT_ALWAYS(m_blurBuffer.IsValid(), "LocalShadowService: CreateBuffer(blur) failed");
			m_blurBufferAddr = gpu::GetBufferAddress(m_blurBuffer);
			AE_ASSERT_ALWAYS(m_blurBufferAddr != 0, "LocalShadowService: blur buffer address is 0");

			m_blurScratchBuffer = gpu::ResourceRegistry::CreateBuffer({
			        .size = kBlurBufSize,
			        .usage = kBlurBufUsage,
			        .debugName = "ShadowBlurScratchBuffer",
			});
			AE_ASSERT_ALWAYS(m_blurScratchBuffer.IsValid(), "LocalShadowService: CreateBuffer(blur scratch) failed");
			m_blurScratchBufferAddr = gpu::GetBufferAddress(m_blurScratchBuffer);
			AE_ASSERT_ALWAYS(m_blurScratchBufferAddr != 0, "LocalShadowService: blur scratch buffer address is 0");
		}

		// -- Create VSM blur compute pipeline (BDA, no descriptors) ---------
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

		// -- Create persistent atlas depth attachment ----------------------
		{
			const gpu::TextureDesc desc{
			        .format = depthFormat,
			        .extent = {ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
			        .usage = gpu::ImageUsage::DepthStencilAttachment,
			        .aspect = gpu::ImageAspect::Depth,
			        .debugName = "LocalShadow.AtlasDepth",
			};
			m_atlasDepthHandle = gpu::ResourceRegistry::CreateTexture(desc);
			AE_ASSERT_ALWAYS(m_atlasDepthHandle.IsValid(), "LocalShadowService: CreateTexture(atlas depth) failed");
			m_atlasDepthImageVk = gpu::ResourceRegistry::ResolveTextureImage(m_atlasDepthHandle);
			m_atlasDepthView = gpu::ResourceRegistry::ResolveTexture(m_atlasDepthHandle).view;
		}

		// Descriptor pool.
		{
		}
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
		m_atlasManager.Shutdown();

		if (m_blurBuffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurBuffer);
			m_blurBuffer = {};
		}
		if (m_blurScratchBuffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurScratchBuffer);
			m_blurScratchBuffer = {};
		}
		m_blurBufferAddr = 0;
		m_blurScratchBufferAddr = 0;

		if (m_blurPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurPipelineHandle);
			m_blurPipelineHandle = {};
		}

		if (m_atlasDepthHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_atlasDepthHandle);
			m_atlasDepthHandle = {};
		}
		m_atlasDepthImageVk = nullptr;
		m_atlasDepthView = nullptr;
	}

	void LocalShadowService::PrepareQueues(const std::uint32_t drawSlot, World& world)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		m_shadowRenderQueue.Clear(drawSlot);
		WorldRenderer::Flush(world, m_shadowRenderQueue);
	}

	void LocalShadowService::BuildFrameShadowData(const RenderFramePacket& packet, const std::uint32_t frameIdx, CameraManager& cameraManager, World& world, FrameConstants& fc)
	{
		AE_PROFILE_ZONE();
		(void) packet;
		(void) world;

		m_atlasManager.Reset();
		m_perLightShadows.clear();

		// Collect and prioritize shadow-casting lights.
		const Camera* mainCam = cameraManager.TryGetMainCamera();
		const glm::vec3 camPos = (mainCam != nullptr) ? mainCam->GetPosition() : glm::vec3(0.0f);

		const auto pointCount = static_cast<std::uint32_t>(packet.pointLights.size());
		const auto spotCount = static_cast<std::uint32_t>(packet.spotLights.size());

		struct ShadowCandidate
		{
			glm::vec3 position;
			float radius;
			std::uint32_t lightIndex;
			std::uint32_t lightType; // 0=point, 1=spot
			float distanceSq;
		};

		std::vector<ShadowCandidate> candidates;

		// Gather point lights (type 0 on CPU = point).
		{
			std::span<const Renderer::PointLight> ptLights = packet.pointLights;
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

		// Gather spot lights (type 1 on CPU = spot).
		{
			std::span<const Renderer::SpotLight> spLights = packet.spotLights;
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

		// Sort by distance (closest first = highest priority), with a stable
		// tie-break so the shadow budget does not reshuffle equal candidates.
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

		// Clamp to budget.
		const std::uint32_t budget = std::min(static_cast<std::uint32_t>(candidates.size()), kMaxLocalShadows);

		// Initialize shadow index mapping - entries are filled during the
		// allocation loop below. Lights not in the budget stay at -1.
		const std::uint32_t totalLights = pointCount + spotCount;
		m_lightShadowIndices.assign(totalLights, glm::vec2(-1.0f, 1.0f));

		// Allocate atlas regions and build per-light data.
		// Track the running index into m_perLightShadows so that the
		// shadow-index mapping below points to the correct entries.
		std::uint32_t shadowDataIdx = 0;

		// Fixed resolution for all shadows - avoids atlas layout shifts when
		// camera distance changes, which causes flickering.
		constexpr std::uint32_t kShadowRes = 512u;

		for (std::uint32_t i = 0; i < budget; ++i)
		{
			const ShadowCandidate& c = candidates[i];

			if (c.lightType == 0u)
			{
				if (shadowDataIdx + kPointLightFaceCount > kMaxRenderedLocalShadowEntries || shadowDataIdx + kPointLightFaceCount > kMaxLocalShadows)
				{
					continue;
				}

				// Point light: six fixed cube-style faces. The small FOV overlap
				// avoids receiver shadows popping at face boundaries.
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
					        .depthBias = 0.005f,
					        .normalBias = 0.015f,
					        .lightType = 1u,
					});
				}

				// Map this point light to the first of its consecutive face entries.
				m_lightShadowIndices[c.lightIndex] = glm::vec2(static_cast<float>(shadowDataIdx), 1.0f);
				shadowDataIdx += kPointLightFaceCount;
			}
			else
			{
				if (shadowDataIdx + 1u > kMaxRenderedLocalShadowEntries || shadowDataIdx + 1u > kMaxLocalShadows)
				{
					break;
				}

				// Spot light: single perspective region.
				ShadowAtlasManager::Region r = m_atlasManager.Allocate(kShadowRes, kShadowRes);
				if (!r.IsValid())
				{
					break;
				}

				std::span<const Renderer::SpotLight> spLights = packet.spotLights;
				const Renderer::SpotLight& src = spLights[c.lightIndex];
				const glm::vec3 lightDir = glm::normalize(src.direction);
				const glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
				const glm::mat4 lightView = glm::lookAt(src.position, src.position + lightDir, up);
				const float fov = std::min(2.0f * (src.outerAngleRad + kSpotShadowFovPaddingRad), glm::radians(175.0f));
				const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(fov, 1.0f, 1.0f, 0.1f, src.radius);

				m_perLightShadows.push_back(PerLightShadow{
				        .viewProj = lightProj * lightView,
				        .region = r,
				        .depthBias = 0.005f,
				        .normalBias = 0.015f,
				        .lightType = 0u, // spot
				});

				// Map this spot light to its single entry.
				m_lightShadowIndices[pointCount + c.lightIndex] = glm::vec2(static_cast<float>(shadowDataIdx), 1.0f);
				shadowDataIdx += 1u;
			}
		}

		// Write ShadowLightData to per-frame GPU buffer.
		const auto shadowCount = static_cast<std::uint32_t>(m_perLightShadows.size());
		const std::uint32_t bufSlot = frameIdx % kMaxFramesInFlight;
		auto mapped = static_cast<ShadowLightData*>(m_shadowDataBuffer[bufSlot].mapped);
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
		}
		gpu::ResourceRegistry::FlushMappedBuffer(m_shadowDataBuffer[bufSlot].handle, 0, static_cast<gpu::DeviceSize>(shadowCount) * sizeof(ShadowLightData));

		// Write per-light frame constants (just viewProj) for atlas rendering.
		auto lightFc = static_cast<FrameConstants*>(m_lightConstantsBuffer[bufSlot].mapped);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			lightFc[i].viewProj = m_perLightShadows[i].viewProj;
			lightFc[i].cameraWorldPos = glm::vec4(camPos, 1.0f);
			lightFc[i].RefreshDerived();
		}
		gpu::ResourceRegistry::FlushMappedBuffer(m_lightConstantsBuffer[bufSlot].handle, 0, static_cast<gpu::DeviceSize>(shadowCount) * sizeof(FrameConstants));

		// Fill FrameConstants for the shader.
		fc.shadowAtlasSlot = m_atlasBindlessSlot;
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
		// Register the atlas as an external image in the render graph.
		m_atlasImage = graph.RegisterImage(m_atlasManager.GetAtlasImage(), m_atlasManager.GetAtlasView(), gpu::ImageAspect::Color);

		// Register the blur BDA buffer.
		m_blurBufferRG = graph.RegisterBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_blurBuffer));
		m_blurScratchBufferRG = graph.RegisterBuffer(gpu::ResourceRegistry::ResolveBufferVkHandle(m_blurScratchBuffer));

		// Create a persistent depth attachment for the atlas render pass.
		m_atlasDepthImage = graph.RegisterImage(m_atlasDepthImageVk, m_atlasDepthView, gpu::ImageAspect::Depth);
	}

	void LocalShadowService::RegisterComputePasses(RenderGraph& graph, CullPass& cullPass)
	{
		// Compute pass: cull draws for local shadow casters.
		graph.AddComputePass("$CullLocalShadowDraws")
		        .DisableAsyncCompute()
		        .HasSideEffects("produces local shadow RenderQueue prepared draw state")
		        .ExecuteCompute(
		                [this, &cullPass](PassContext& ctx)
		                {
			                m_shadowRenderQueue.SetDebugForceVisible(true);
			                m_shadowRenderQueue.PrepareAndDispatch(ctx.recorder, ctx.frameConstantsAddr, cullPass.GetSinglePipeline(), ctx.frameSlot);
		                })
		        .OnDebugDisabled([this](PassContext& ctx) { m_shadowRenderQueue.DiscardPending(ctx.frameSlot); });
	}

	void LocalShadowService::RegisterGraphicsPasses(RenderGraph& graph)
	{
		// Graphics pass: render all shadow casters into the atlas with per-light scissoring.
		graph.AddPass("$LocalShadowAtlasRender")
		        .DependsOn("$CullLocalShadowDraws")
		        .WriteColor(m_atlasImage, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(1.0f, 1.0f, 1.0f, 1.0f))
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

		// -- VSM blur passes (BDA, no descriptors) ---------------------------
		// Flow: copy atlas → buffer, H-blur (in-place via LDS), V-blur (in-place via LDS), copy buffer → atlas.
		// All synchronization is handled by the render graph via transfer access types.

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

			                const auto atlasImage = m_atlasManager.GetAtlasImage();
			                const auto blurVkBuf = gpu::ResourceRegistry::ResolveBufferVkHandle(m_blurBuffer);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.CopyImageToBuffer(atlasImage, blurVkBuf, gpu::ImageLayout::TransferSrc, gpu::ImageAspect::Color, bounds.width, bounds.height, 0, static_cast<std::int32_t>(bounds.x), static_cast<std::int32_t>(bounds.y));
		                });

		graph.AddComputePass("$VSMBlurH")
		        .DisableAsyncCompute()
		        .ReadBuffer(m_blurBufferRG)
		        .WriteBuffer(m_blurScratchBufferRG)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return;
			                }

			                const auto blurPipeline = gpu::ResourceRegistry::ResolvePipeline(m_blurPipelineHandle);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(const_cast<void*>(blurPipeline.state));
			                const BlurPushConstants hPc{
			                        .atlasWidth = bounds.width,
			                        .atlasHeight = bounds.height,
			                        .blurOffsetX = 0,
			                        .blurOffsetY = 0,
			                        .isHorizontal = 1u,
			                        ._pad0 = 0.0f,
			                        ._pad1 = 0.0f,
			                        ._pad2 = 0.0f,
			                        .srcAddr = m_blurBufferAddr,
			                        .dstAddr = m_blurScratchBufferAddr,
			                };
			                cmd.PushDataRaw(0, std::as_bytes(std::span{&hPc, 1}));
			                cmd.Dispatch((bounds.width + 15u) / 16u, (bounds.height + 15u) / 16u, 1u);
		                });

		graph.AddComputePass("$VSMBlurV")
		        .DisableAsyncCompute()
		        .ReadBuffer(m_blurScratchBufferRG)
		        .WriteBuffer(m_blurBufferRG)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return;
			                }

			                const auto blurPipeline = gpu::ResourceRegistry::ResolvePipeline(m_blurPipelineHandle);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(const_cast<void*>(blurPipeline.state));
			                const BlurPushConstants vPc{
			                        .atlasWidth = bounds.width,
			                        .atlasHeight = bounds.height,
			                        .blurOffsetX = 0,
			                        .blurOffsetY = 0,
			                        .isHorizontal = 0u,
			                        ._pad0 = 0.0f,
			                        ._pad1 = 0.0f,
			                        ._pad2 = 0.0f,
			                        .srcAddr = m_blurScratchBufferAddr,
			                        .dstAddr = m_blurBufferAddr,
			                };
			                cmd.PushDataRaw(0, std::as_bytes(std::span{&vPc, 1}));
			                cmd.Dispatch((bounds.width + 15u) / 16u, (bounds.height + 15u) / 16u, 1u);
		                });

		graph.AddComputePass("$VSMCopyToAtlas")
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

			                const auto atlasImage = m_atlasManager.GetAtlasImage();
			                const auto blurVkBuf = gpu::ResourceRegistry::ResolveBufferVkHandle(m_blurBuffer);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.CopyBufferToImage(blurVkBuf, atlasImage, gpu::ImageLayout::TransferDst, gpu::ImageAspect::Color, bounds.width, bounds.height, 0, static_cast<std::int32_t>(bounds.x), static_cast<std::int32_t>(bounds.y));
		                });
	}
} // namespace aether
