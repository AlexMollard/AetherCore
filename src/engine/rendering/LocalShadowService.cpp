#include "rendering/LocalShadowService.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>

#include "camera/CameraManager.hpp"
#include "utils/Profiler.hpp"
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
#include "scene/Scene.hpp"
#include "scene/World.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/ShaderUtils.hpp"
#include "vulkan/Swapchain.hpp"
#include "vulkan/VulkanContext.hpp"

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
	};

	static_assert(sizeof(BlurPushConstants) == 32, "BlurPushConstants must be 32 bytes");

	void LocalShadowService::Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines)
	{
		AE_PROFILE_ZONE();
		auto device = static_cast<gpu::Device>(context.GetDevice().device);
		auto allocator = static_cast<gpu::Allocator>(context.GetAllocator());

		m_atlasManager.Initialize(context, bindless);
		m_atlasBindlessSlot = m_atlasManager.GetBindlessSlot();

		m_shadowRenderQueue.Initialize(device, allocator, pipelines, RenderQueueConfig{.maxDraws = 4096, .maxBatches = 512, .maxAnimationDraws = 1024u});
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu); // Test: PoseInit + AnimSample

		// Create the shadow depth pipeline (reads VP from per-light FrameConstants via BDA).
		const gpu::Format depthFormat = swapchain.GetDepthFormat();
		AE_EXPECT_OR_THROW(pipeline,
		        GraphicsPipeline::Create(device,
		                context.GetPipelineCache(),
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

		// -- Create blur scratch image --------------------------------------
		m_blurScratchHandle = gpu::ResourceRegistry::CreateTexture({
		        .format = ShadowAtlasManager::kAtlasFormat,
		        .extent = {ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
		        .usage = gpu::ImageUsage::Storage | gpu::ImageUsage::Sampled,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = "ShadowBlurScratch",
		});

		// -- Create VSM blur compute pipeline -------------------------------
		// Descriptor set layout: binding 0 = RWTexture2D (storage), binding 1 = Texture2D (combined sampler).
		{
			const gpu::GpuDescriptorSetLayoutBinding bindings[2] = {
			        {
			                .binding = 0,
			                .descriptorType = gpu::DescriptorType::StorageImage,
			                .descriptorCount = 1,
			                .stageFlags = gpu::ShaderStage::Compute,
			        },
			        {
			                .binding = 1,
			                .descriptorType = gpu::DescriptorType::CombinedImageSampler,
			                .descriptorCount = 1,
			                .stageFlags = gpu::ShaderStage::Compute,
			        },
			};
			m_blurDescriptorSetLayout = gpu::Factory::CreateDescriptorSetLayout(device,
			        {
			                .bindings = bindings,
			                .pushDescriptor = true,
			        });
			AE_ASSERT_ALWAYS(m_blurDescriptorSetLayout != nullptr, "Failed to create blur descriptor set layout");
		}

		// Pipeline layout with push constants.
		{
			const gpu::PushConstantRange pcRange{
			        .stageFlags = gpu::ShaderStage::Compute,
			        .offset = 0,
			        .size = sizeof(BlurPushConstants),
			};
			const std::array<gpu::DescriptorSetLayout, 1> setLayouts{m_blurDescriptorSetLayout};
			m_blurPipelineLayout = gpu::Factory::CreatePipelineLayout(device,
			        {
			                .setLayouts = setLayouts,
			                .pushConstantRanges = std::span<const gpu::PushConstantRange>(&pcRange, 1),
			        });
			AE_ASSERT_ALWAYS(m_blurPipelineLayout != nullptr, "Failed to create blur pipeline layout");
		}

		// Compute pipeline via resource registry.
		m_blurPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(device,
		        context.GetPipelineCache(),
		        gpu::ComputePipelineDesc{
		                .shaderVfsPath = "shaders://vsm_blur.spv",
		                .shaderEntry = "main",
		                .pushConstantSize = static_cast<std::uint32_t>(sizeof(BlurPushConstants)),
		                .debugName = "VSMBlur",
		                .existingLayout = m_blurPipelineLayout,
		        });
		if (!m_blurPipelineHandle.IsValid())
		{
			AE_ASSERT_ALWAYS(false, "Failed to create VSM blur compute pipeline");
		}

		// Sampler for blur input (nearest clamp-to-edge - texel fetch, sampler unused).
		// Cached in BindlessManager - one entry for the lifetime of the bindless pool.
		{
			const auto samplerResult = bindless.GetOrCreateSampler(gpu::Filter::Nearest, gpu::SamplerMipmapMode::Nearest, gpu::SamplerAddressMode::ClampToEdge);
			AE_ASSERT_ALWAYS(samplerResult, "Failed to create blur sampler");
			m_blurSampler = samplerResult.value();
		}

		// Descriptor pool.
		{
		}
	}

	void LocalShadowService::Shutdown(gpu::Device device)
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

		if (m_blurScratchHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurScratchHandle);
			m_blurScratchHandle = {};
		}

		if (m_blurPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurPipelineHandle);
			m_blurPipelineHandle = {};
		}
		if (m_blurPipelineLayout != nullptr)
		{
			gpu::Factory::DestroyPipelineLayout(device, m_blurPipelineLayout);
			m_blurPipelineLayout = nullptr;
		}
		if (m_blurSampler != nullptr)
		{
			// Cached in BindlessManager - no explicit destroy needed; lives for the
			// lifetime of the bindless pool. Clear the handle to indicate "not owned".
			m_blurSampler = nullptr;
		}
		if (m_blurDescriptorSetLayout != nullptr)
		{
			gpu::Factory::DestroyDescriptorSetLayout(device, m_blurDescriptorSetLayout);
			m_blurDescriptorSetLayout = nullptr;
		}
	}

	void LocalShadowService::PrepareQueues(const std::uint32_t drawSlot, Scene& scene, World& world)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		m_shadowRenderQueue.Clear(drawSlot);
		WorldRenderer::Flush(scene, m_shadowRenderQueue);
		WorldRenderer::Flush(world, m_shadowRenderQueue);
	}

	void LocalShadowService::BuildFrameShadowData(const RenderFramePacket& packet, const std::uint32_t frameIdx, CameraManager& cameraManager, Scene& scene, World& world, FrameConstants& fc)
	{
		AE_PROFILE_ZONE();
		// Re-populate the shadow render queue to pick up any mid-frame changes
		// to MeshComponent::mesh pointers made by game systems after the initial
		// PrepareQueues call.
		m_shadowRenderQueue.SetWriteSlot(packet.drawSlot);
		m_shadowRenderQueue.Clear(packet.drawSlot);
		WorldRenderer::Flush(scene, m_shadowRenderQueue);
		WorldRenderer::Flush(world, m_shadowRenderQueue);

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

		// Sort by distance (closest first = highest priority).
		std::ranges::sort(candidates, [](const ShadowCandidate& a, const ShadowCandidate& b) { return a.distanceSq < b.distanceSq; });

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
				// Point light: two fixed hemisphere faces (upper +Y, lower -Y).
				// Using fixed world-space directions instead of camera-relative
				// ones eliminates flickering when the camera moves.
				const std::uint32_t faceRes = std::max(kShadowRes / 2u, 64u);

				ShadowAtlasManager::Region r0 = m_atlasManager.Allocate(faceRes, faceRes);
				ShadowAtlasManager::Region r1 = m_atlasManager.Allocate(faceRes, faceRes);
				if (!r0.IsValid() || !r1.IsValid())
				{
					break;
				}

				// Upper hemisphere: look up from the light.
				{
					const glm::mat4 lightView = glm::lookAt(c.position, c.position + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
					const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(glm::radians(170.0f), 1.0f, 1.0f, 0.1f, c.radius);

					m_perLightShadows.push_back(PerLightShadow{
					        .viewProj = lightProj * lightView,
					        .region = r0,
					        .depthBias = 0.005f,
					        .normalBias = 0.015f,
					        .lightType = 1u, // point
					});
				}

				// Lower hemisphere: look down from the light.
				{
					const glm::mat4 lightView = glm::lookAt(c.position, c.position + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
					const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(glm::radians(170.0f), 1.0f, 1.0f, 0.1f, c.radius);

					m_perLightShadows.push_back(PerLightShadow{
					        .viewProj = lightProj * lightView,
					        .region = r1,
					        .depthBias = 0.005f,
					        .normalBias = 0.015f,
					        .lightType = 1u, // point (same type, 2nd entry)
					});
				}

				// Map this point light to the first of its 2 consecutive entries.
				m_lightShadowIndices[c.lightIndex] = glm::vec2(static_cast<float>(shadowDataIdx), 1.0f);
				shadowDataIdx += 2u;
			}
			else
			{
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
				const float fov = 2.0f * src.outerAngleRad;
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
		}
		gpu::ResourceRegistry::FlushMappedBuffer(m_lightConstantsBuffer[bufSlot].handle, 0, static_cast<gpu::DeviceSize>(shadowCount) * sizeof(FrameConstants));

		// Fill FrameConstants for the shader.
		fc.shadowAtlasSlot = m_atlasBindlessSlot;
		fc.shadowLightCount = shadowCount;
		fc.shadowLightDataAddr = m_shadowDataBuffer[bufSlot].address;
	}

	void LocalShadowService::RegisterPasses(RenderGraph& graph, gpu::Device device, CullPass& cullPass, gpu::Format depthFormat)
	{
		(void) device;
		SetupPassResources(graph, depthFormat);
		RegisterComputePasses(graph, cullPass);
		RegisterGraphicsPasses(graph);
	}

	void LocalShadowService::SetupPassResources(RenderGraph& graph, gpu::Format depthFormat)
	{
		// Register the atlas as an external image in the render graph.
		m_atlasImage = graph.RegisterImage(m_atlasManager.GetAtlasImage(), m_atlasManager.GetAtlasView(), gpu::ImageAspect::Color);

		// Register the blur scratch image.
		auto scratchView = gpu::ResourceRegistry::ResolveTexture(m_blurScratchHandle).view;
		auto scratchImage = gpu::ResourceRegistry::ResolveTextureImage(m_blurScratchHandle);
		m_blurScratchImage = graph.RegisterImage(scratchImage, scratchView, gpu::ImageAspect::Color);

		// Create a transient depth attachment for the atlas render pass.
		m_atlasDepthImage = graph.CreateTransientDepth(depthFormat, gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight}, gpu::ImageUsage::DepthStencilAttachment);
	}

	void LocalShadowService::RegisterComputePasses(RenderGraph& graph, CullPass& cullPass)
	{
		// Compute pass: cull draws for local shadow casters.
		graph.AddComputePass("$CullLocalShadowDraws")
		        .ExecuteCompute(
		                [this, &cullPass](PassContext& ctx)
		                {
			                if (m_shadowRenderQueue.IsEmpty(ctx.frameIndex % RenderQueue::kFramesInFlight))
			                {
				                return;
			                }
			                m_shadowRenderQueue.SetDebugForceVisible(true);
			                m_shadowRenderQueue.PrepareAndDispatch(ctx.recorder, ctx.frameConstantsAddr, cullPass.GetSinglePipeline(), cullPass.GetSingleLayout(), ctx.frameIndex);
		                });
	}

	void LocalShadowService::RegisterGraphicsPasses(RenderGraph& graph)
	{
		// Graphics pass: render all shadow casters into the atlas with per-light scissoring.
		graph.AddPass("$LocalShadowAtlasRender")
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

				                const gpu::DeviceAddress lightFcAddr = m_lightConstantsBuffer[ctx.frameIndex % kMaxFramesInFlight].address + static_cast<gpu::DeviceSize>(li) * sizeof(FrameConstants);
				                m_shadowRenderQueue.FlushDrawWithFrameAddr(cmd, nullptr, nullptr, lightFcAddr, &m_shadowPipeline);
			                }

			                m_shadowRenderQueue.Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
		                });

		// -- VSM blur passes ------------------------------------------------
		// Horizontal blur: read atlas (sampled), write scratch (storage).
		// Descriptors set up once in Initialize() - no per-frame updates needed.
		graph.AddComputePass("$VSMBlurH")
		        .ReadTexture(m_atlasImage)
		        .WriteStorageImage(m_blurScratchImage)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return; // Nothing allocated, skip blur
			                }

			                const auto blurPipeline = gpu::ResourceRegistry::ResolvePipeline(m_blurPipelineHandle);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(blurPipeline.pipeline, blurPipeline.layout);

			                const auto scratchResolved = gpu::ResourceRegistry::ResolveTexture(m_blurScratchHandle);
			                const gpu::GpuDescriptorImageInfo hStorageInfo{
			                        .sampler = nullptr,
			                        .imageView = scratchResolved.view,
			                        .imageLayout = gpu::ImageLayout::General,
			                };
			                const gpu::GpuDescriptorImageInfo hSampledInfo{
			                        .sampler = m_blurSampler,
			                        .imageView = m_atlasManager.GetAtlasView(),
			                        .imageLayout = gpu::ImageLayout::ShaderReadOnly,
			                };
			                const gpu::GpuWriteDescriptorSet hWrites[]{
			                        {
			                                .dstBinding = 0,
			                                .descriptorCount = 1,
			                                .descriptorType = gpu::DescriptorType::StorageImage,
			                                .imageInfo = &hStorageInfo,
			                        },
			                        {
			                                .dstBinding = 1,
			                                .descriptorCount = 1,
			                                .descriptorType = gpu::DescriptorType::CombinedImageSampler,
			                                .imageInfo = &hSampledInfo,
			                        },
			                };
			                cmd.PushDescriptorSet(gpu::PipelineBindPoint::Compute, blurPipeline.layout, 0, std::span<const gpu::GpuWriteDescriptorSet>(hWrites));

			                const BlurPushConstants hPc{.atlasWidth = bounds.width, .atlasHeight = bounds.height, .blurOffsetX = bounds.x, .blurOffsetY = bounds.y, .isHorizontal = 1u, ._pad0 = 0.0f, ._pad1 = 0.0f, ._pad2 = 0.0f};
			                cmd.PushConstantsRaw(blurPipeline.layout, gpu::ShaderStage::Compute, 0, std::as_bytes(std::span{&hPc, 1}));

			                cmd.Dispatch((bounds.width + 15u) / 16u, (bounds.height + 15u) / 16u, 1u);
		                });

		// Vertical blur: read scratch (sampled), write atlas (storage).
		graph.AddComputePass("$VSMBlurV")
		        .ReadTexture(m_blurScratchImage)
		        .WriteStorageImage(m_atlasImage)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                const auto bounds = m_atlasManager.GetUsedBounds();
			                if (bounds.width == 0 || bounds.height == 0)
			                {
				                return; // Nothing allocated, skip blur
			                }

			                const auto blurPipeline = gpu::ResourceRegistry::ResolvePipeline(m_blurPipelineHandle);

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(blurPipeline.pipeline, blurPipeline.layout);

			                const auto scratchResolvedV = gpu::ResourceRegistry::ResolveTexture(m_blurScratchHandle);
			                const gpu::GpuDescriptorImageInfo vStorageInfo{
			                        .sampler = nullptr,
			                        .imageView = m_atlasManager.GetAtlasView(),
			                        .imageLayout = gpu::ImageLayout::General,
			                };
			                const gpu::GpuDescriptorImageInfo vSampledInfo{
			                        .sampler = m_blurSampler,
			                        .imageView = scratchResolvedV.view,
			                        .imageLayout = gpu::ImageLayout::ShaderReadOnly,
			                };
			                const gpu::GpuWriteDescriptorSet vWrites[]{
			                        {
			                                .dstBinding = 0,
			                                .descriptorCount = 1,
			                                .descriptorType = gpu::DescriptorType::StorageImage,
			                                .imageInfo = &vStorageInfo,
			                        },
			                        {
			                                .dstBinding = 1,
			                                .descriptorCount = 1,
			                                .descriptorType = gpu::DescriptorType::CombinedImageSampler,
			                                .imageInfo = &vSampledInfo,
			                        },
			                };
			                cmd.PushDescriptorSet(gpu::PipelineBindPoint::Compute, blurPipeline.layout, 0, std::span<const gpu::GpuWriteDescriptorSet>(vWrites));

			                const BlurPushConstants vPc{.atlasWidth = bounds.width, .atlasHeight = bounds.height, .blurOffsetX = bounds.x, .blurOffsetY = bounds.y, .isHorizontal = 0u, ._pad0 = 0.0f, ._pad1 = 0.0f, ._pad2 = 0.0f};
			                cmd.PushConstantsRaw(blurPipeline.layout, gpu::ShaderStage::Compute, 0, std::as_bytes(std::span{&vPc, 1}));

			                cmd.Dispatch((bounds.width + 15u) / 16u, (bounds.height + 15u) / 16u, 1u);
		                });
	}
} // namespace aether
