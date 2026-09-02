#include "rendering/LocalShadowService.hpp"
#include "utils/Logger.hpp"

#include <algorithm>
#include <ranges>
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
	constexpr std::uint32_t kPointShadowFaceRes = 384u;
	// which needs no separate coverage). Radial depth is face-invariant, so the
	constexpr float kPointLightFovDeg = 90.0f;

	// Guard band, in texels, around each cube face. The six faces partition direction
	// space at exactly 90 degrees, so a receiver sitting on a boundary projects to the
	// very edge of its face - and once normal-offset bias nudges it, a hair past the
	// edge, where the sampler has to call it unshadowed. That reads as a bright seam
	// along every face boundary, and with several shadowed point lights the seams cross
	// into a grid. Rendering each face slightly wider than 90 degrees puts the boundary
	// well inside valid texels and gives the filter kernel room to land.
	constexpr float kPointShadowGuardTexels = 6.0f;

	// FOV that covers the 90-degree cone plus the guard band on each side.
	// World units one texel spans per unit of distance from the light.
	float TexelScaleFor(const float fovRad, const std::uint32_t resolution)
	{
		return 2.0f * glm::tan(fovRad * 0.5f) / static_cast<float>(resolution);
	}

	const float kPointLightPaddedFovRad = 2.0f * std::atan(glm::tan(glm::radians(kPointLightFovDeg) * 0.5f) * ((static_cast<float>(kPointShadowFaceRes) + 2.0f * kPointShadowGuardTexels) / static_cast<float>(kPointShadowFaceRes)));
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
	void LocalShadowService::Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines)
	{
		AE_PROFILE_ZONE();
		auto* device = static_cast<gpu::Device>(context.GetDevice().device);
		(void) bindless;

		m_shadowRenderQueue.Initialize(pipelines, RenderQueueConfig{.maxDraws = 4096, .maxBatches = 512, .maxAnimationDraws = 1024u, .debugName = "LocalShadow"});
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu);

		m_bindless = &bindless;

		const gpu::Format depthFormat = swapchain.GetDepthFormat();
		AE_EXPECT_OR_THROW(pipeline,
		        GraphicsPipeline::Create(device,
		                {
		                        .shaderVfsPath = "shaders://local_shadow_depth.spv",
		                        .colorFormat = ShadowAtlasManager::kAtlasFormat,
		                        .depthFormat = depthFormat,
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = gpu::CompareOp::LessOrEqual,
		                        .cullMode = gpu::CullMode::Back,
		                        .debugName = "LocalShadow.Depth",
		                        .descriptorHeapMappings = bindless.GetDescriptorHeapMappings(),
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
		// Depth only, so nothing blends and the transparent ordering cannot matter.
		WorldRenderer::Flush(world, m_shadowRenderQueue, glm::vec3(0.0f), /*shadowPass*/ true);
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
			float sourceRadius;
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
				        .sourceRadius = ptLights[i].sourceRadius,
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
				        .sourceRadius = spLights[i].sourceRadius,
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
					const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(kPointLightPaddedFovRad, 1.0f, 1.0f, 0.1f, c.radius);
					m_perLightShadows.push_back(PerLightShadow{
					        .viewProj = lightProj * lightView,
					        .region = regions[face],
					        .texelScale = TexelScaleFor(kPointLightPaddedFovRad, kPointShadowFaceRes),
					        .sourceRadius = c.sourceRadius,
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
				        .texelScale = TexelScaleFor(fov, kSpotShadowRes),
				        .sourceRadius = c.sourceRadius,
				        .lightType = 0u,
				        .lightPosRange = glm::vec4(src.position, src.radius),
				});

				m_lightShadowIndices[pointCount + c.lightIndex] = glm::vec2(static_cast<float>(shadowDataIdx), 1.0f);
				shadowDataIdx += 1u;
			}
		}

		const auto shadowCount = static_cast<std::uint32_t>(m_perLightShadows.size());

		// Running out of atlas room only breaks the loop above, so a light that asked for a
		// shadow quietly renders without one and nothing says why. A point light needs six
		// faces, so the atlas holds twelve of them and the thirteenth is where this starts.
		// Logged only when the number changes - this runs every frame.
		const auto served = static_cast<std::uint32_t>(std::ranges::count_if(m_lightShadowIndices, [](const glm::vec2& v) { return v.x >= 0.0f; }));
		const std::uint32_t dropped = budget > served ? budget - served : 0u;
		if (dropped != m_lastDroppedShadowCasters)
		{
			m_lastDroppedShadowCasters = dropped;
			if (dropped > 0)
			{
				AE_WARN(LogCategory::Render, "Local shadow atlas is full: {} of {} shadow-casting lights got no shadow. They still light the scene, they just stop casting.", dropped, budget);
			}
		}

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
			mapped[i].texelScale = pls.texelScale;
			mapped[i].sourceRadius = pls.sourceRadius;
			mapped[i].lightType = pls.lightType;
			mapped[i].lightPosRange = pls.lightPosRange;
		}
		gpu::ResourceRegistry::FlushMappedBuffer(m_shadowDataBuffer[bufSlot].handle, 0, static_cast<gpu::DeviceSize>(shadowCount) * sizeof(ShadowLightData));

		auto* lightFc = static_cast<FrameConstants*>(m_lightConstantsBuffer[bufSlot].mapped);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			lightFc[i].viewProj = m_perLightShadows[i].viewProj;
			lightFc[i].cameraWorldPos = m_perLightShadows[i].lightPosRange;
			// The caster's alpha test looks its material up through this pointer. These constants
			// are built from scratch per light rather than copied from the frame's, so a field the
			// shadow shader needs has to be carried across explicitly - left at zero it is a null
			// dereference on the GPU, not a missing texture.
			lightFc[i].materialBufferAddr = fc.materialBufferAddr;
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
		// Written by $LocalShadowAtlasRender and read for the last time by $EngineForward -
		// 64 MiB that never outlives the frame. PCSS samples the atlas in place, so there is
		// no post-process chain and no scratch copy of it.
		m_atlasImage = graph.CreateTransientColor(ShadowAtlasManager::kAtlasFormat,
		        gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
		        gpu::ImageUsage::Sampled);
		m_atlasBindlessSlot = graph.EnsureBindlessSampled(m_atlasImage);
		if (m_atlasBindlessSlot == 0xFFFFFFFFu)
		{
			Throw(AetherError::Engine("LocalShadowService: shadow atlas bindless registration failed"));
		}

		m_atlasDepthImage = graph.CreateTransientDepth(m_atlasDepthFormat, gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight});
		(void) graph.GetBlackboard().DeclareGraphProduct<LocalShadowProduct>(std::string{kFrameProductLocalShadows},
		        LocalShadowProduct{
		                .atlasImage = m_atlasImage,
		                .atlasDepthImage = m_atlasDepthImage,
		                .atlasBindlessSlot = m_atlasBindlessSlot,
		                .atlasExtent = gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
		                .atlasFormat = ShadowAtlasManager::kAtlasFormat,
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
		        .ProducesProduct<LocalShadowProduct>(kFrameProductLocalShadows)
		        .ConsumesDrawList(m_shadowDrawList)
		        .WriteColor(m_atlasImage, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(1.0f, 0.0f, 0.0f, 0.0f))
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
			                // The caster's alpha test reads the albedo through the heap.
			                if (m_bindless != nullptr)
			                {
				                m_bindless->CmdBindGlobalResources(cmd);
			                }
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

	}
} // namespace aether
