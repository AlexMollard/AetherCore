#include "rendering/LocalShadowService.hpp"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>

#include "camera/CameraManager.hpp"
#include "utils/Profiler.hpp"
#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
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
		const VkDevice device = context.GetDevice().device;
		const VmaAllocator allocator = context.GetAllocator();

		m_atlasManager.Initialize(context, bindless);
		m_atlasBindlessSlot = m_atlasManager.GetBindlessSlot();

		m_shadowRenderQueue.Initialize(device, allocator, pipelines, RenderQueueConfig{.maxDraws = 4096, .maxBatches = 512, .maxAnimationDraws = 1024u});
		m_shadowRenderQueue.SetDebugDisableAnimation(false);
		m_shadowRenderQueue.SetDebugAnimPassMask(0xFFFFFFFFu); // Test: PoseInit + AnimSample
		m_shadowRenderQueue.SetTracyVkCtx(context.GetTracyVkCtx());

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
		                }));
		m_shadowPipeline = std::move(pipeline);

		m_perLightShadows.reserve(kMaxLocalShadows);
		m_lightShadowIndices.reserve(4096);

		// Allocate per-frame GPU buffers for per-light shadow data and light constants (double-buffered).
		constexpr VkBufferUsageFlags kSsboBda = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			{
				AE_EXPECT_OR_THROW(sdBuf, UniqueBuffer::CreateMapped(allocator, device, static_cast<VkDeviceSize>(kMaxLocalShadows) * sizeof(ShadowLightData), kSsboBda, "LocalShadow.ShadowData"));
				m_shadowDataBuffer[i] = std::move(sdBuf);
				m_shadowDataAddr[i] = m_shadowDataBuffer[i].GetDeviceAddress();
			}
			{
				AE_EXPECT_OR_THROW(lcBuf, UniqueBuffer::CreateMapped(allocator, device, static_cast<VkDeviceSize>(kMaxLocalShadows) * sizeof(FrameConstants), kSsboBda, "LocalShadow.LightConstants"));
				m_lightConstantsBuffer[i] = std::move(lcBuf);
				m_lightConstantsAddr[i] = m_lightConstantsBuffer[i].GetDeviceAddress();
			}
		}

		// ── Create blur scratch image ──────────────────────────────────────
		AE_EXPECT_OR_THROW(scratchImg,
		        UniqueImage::Create(device,
		                allocator,
		                {
		                        .extent = {ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight},
		                        .format = ShadowAtlasManager::kAtlasFormat,
		                        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                        .debugName = "ShadowBlurScratch",
		                }));
		m_blurScratch = std::move(scratchImg);

		// ── Create VSM blur compute pipeline ───────────────────────────────
		// Descriptor set layout: binding 0 = RWTexture2D (storage), binding 1 = Texture2D (combined sampler).
		{
			VkDescriptorSetLayoutBinding bindings[2]{};
			bindings[0].binding = 0;
			bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			bindings[0].descriptorCount = 1;
			bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings[1].binding = 1;
			bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			bindings[1].descriptorCount = 1;
			bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

			VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
			dslci.bindingCount = 2;
			dslci.pBindings = bindings;
			AE_ASSERT_ALWAYS(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &m_blurDescriptorSetLayout) == VK_SUCCESS, "Failed to create blur descriptor set layout");
		}

		// Pipeline layout with push constants.
		{
			VkPushConstantRange pcRange{};
			pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			pcRange.offset = 0;
			pcRange.size = sizeof(BlurPushConstants);

			VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			plci.setLayoutCount = 1;
			plci.pSetLayouts = &m_blurDescriptorSetLayout;
			plci.pushConstantRangeCount = 1;
			plci.pPushConstantRanges = &pcRange;

			VkPipelineLayout vkLayout = VK_NULL_HANDLE;
			AE_ASSERT_ALWAYS(vkCreatePipelineLayout(device, &plci, nullptr, &vkLayout) == VK_SUCCESS, "Failed to create blur pipeline layout");
			m_blurPipelineLayout = static_cast<gpu::PipelineLayout>(vkLayout);
		}

		// Compute pipeline via resource registry.
		m_blurPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(static_cast<gpu::Device>(device),
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
		{
			VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sci.magFilter = VK_FILTER_NEAREST;
			sci.minFilter = VK_FILTER_NEAREST;
			sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			AE_ASSERT_ALWAYS(vkCreateSampler(device, &sci, nullptr, &m_blurSampler) == VK_SUCCESS, "Failed to create blur sampler");
		}

		// Descriptor pool.
		{
		}
	}

	void LocalShadowService::Shutdown(VkDevice device)
	{
		AE_PROFILE_ZONE();
		m_shadowRenderQueue.Shutdown();
		m_shadowPipeline.Destroy();
		for (auto& buf: m_shadowDataBuffer)
		{
			buf.Reset();
		}
		for (auto& buf: m_lightConstantsBuffer)
		{
			buf.Reset();
		}
		m_atlasManager.Shutdown();

		m_blurScratch.Reset();

		if (m_blurPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_blurPipelineHandle);
			m_blurPipelineHandle = {};
		}
		if (m_blurPipelineLayout != nullptr)
		{
			vkDestroyPipelineLayout(device, static_cast<VkPipelineLayout>(m_blurPipelineLayout), nullptr);
			m_blurPipelineLayout = nullptr;
		}
		if (m_blurSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(device, m_blurSampler, nullptr);
			m_blurSampler = VK_NULL_HANDLE;
		}
		if (m_blurDescriptorSetLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, m_blurDescriptorSetLayout, nullptr);
			m_blurDescriptorSetLayout = VK_NULL_HANDLE;
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

		const std::uint32_t pointCount = static_cast<std::uint32_t>(packet.pointLights.size());
		const std::uint32_t spotCount = static_cast<std::uint32_t>(packet.spotLights.size());

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
		std::sort(candidates.begin(), candidates.end(), [](const ShadowCandidate& a, const ShadowCandidate& b) { return a.distanceSq < b.distanceSq; });

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
		const std::uint32_t shadowCount = static_cast<std::uint32_t>(m_perLightShadows.size());
		const std::uint32_t bufSlot = frameIdx % kMaxFramesInFlight;
		ShadowLightData* mapped = static_cast<ShadowLightData*>(m_shadowDataBuffer[bufSlot].GetAllocationInfo().pMappedData);
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
		AE_EXPECT_OR_THROW_VOID(m_shadowDataBuffer[bufSlot].FlushMapped());

		// Write per-light frame constants (just viewProj) for atlas rendering.
		FrameConstants* lightFc = static_cast<FrameConstants*>(m_lightConstantsBuffer[bufSlot].GetAllocationInfo().pMappedData);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			lightFc[i].viewProj = m_perLightShadows[i].viewProj;
			lightFc[i].cameraWorldPos = glm::vec4(camPos, 1.0f);
		}
		AE_EXPECT_OR_THROW_VOID(m_lightConstantsBuffer[bufSlot].FlushMapped());

		// Fill FrameConstants for the shader.
		fc.shadowAtlasSlot = m_atlasBindlessSlot;
		fc.shadowLightCount = shadowCount;
		fc.shadowLightDataAddr = m_shadowDataAddr[bufSlot];
	}

	void LocalShadowService::RegisterPasses(RenderGraph& graph, BindlessManager& bindless, VkDevice device, CullPass& cullPass, gpu::Format depthFormat)
	{
		AE_PROFILE_ZONE();
		(void) bindless;
		(void) device;
		// Register the atlas as an external image in the render graph.
		m_atlasImage = graph.RegisterImage(m_atlasManager.GetAtlasImage().Get(), m_atlasManager.GetAtlasView(), VK_IMAGE_ASPECT_COLOR_BIT);

		// Register the blur scratch image.
		m_blurScratchImage = graph.RegisterImage(m_blurScratch.Get(), m_blurScratch.GetDefaultView(), VK_IMAGE_ASPECT_COLOR_BIT);

		// Create a transient depth attachment for the atlas render pass.
		RGImage atlasDepth = graph.CreateTransientDepth(depthFormat, gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight}, gpu::ImageUsage::DepthStencilAttachment);

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

		// Graphics pass: render all shadow casters into the atlas with per-light scissoring.
		graph.AddPass("$LocalShadowAtlasRender")
		        .WriteColor(m_atlasImage, gpu::LoadOp::Clear, gpu::StoreOp::Store, ClearColorValue(1.0f, 1.0f, 1.0f, 1.0f))
		        .WriteDepth(atlasDepth, gpu::LoadOp::Clear, gpu::StoreOp::DontCare, ClearDepthValue(1.0f))
		        .SetExtent(gpu::Extent2D{ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight})
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (m_perLightShadows.empty())
			                {
				                return;
			                }

			                gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());
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

				                const gpu::DeviceAddress lightFcAddr = m_lightConstantsAddr[ctx.frameIndex % kMaxFramesInFlight] + static_cast<VkDeviceSize>(li) * sizeof(FrameConstants);
				                m_shadowRenderQueue.FlushDrawWithFrameAddr(cmd, nullptr, nullptr, lightFcAddr, &m_shadowPipeline);
			                }

			                m_shadowRenderQueue.Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
		                });

		// ── VSM blur passes ────────────────────────────────────────────────
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

			                gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());
			                cmd.BindComputePipeline(blurPipeline.pipeline, blurPipeline.layout);

			                const VkDescriptorImageInfo hStorageInfo{
			                        .sampler = VK_NULL_HANDLE,
			                        .imageView = m_blurScratch.GetDefaultView(),
			                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			                };
			                const VkDescriptorImageInfo hSampledInfo{
			                        .sampler = m_blurSampler,
			                        .imageView = m_atlasManager.GetAtlasView(),
			                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                };
			                const VkWriteDescriptorSet hWrites[]{
			                        {
			                                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                                .dstBinding = 0,
			                                .descriptorCount = 1,
			                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			                                .pImageInfo = &hStorageInfo,
			                        },
			                        {
			                                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                                .dstBinding = 1,
			                                .descriptorCount = 1,
			                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			                                .pImageInfo = &hSampledInfo,
			                        },
			                };
			                cmd.PushDescriptorSet(gpu::PipelineBindPoint::Compute, blurPipeline.layout, 0, 2, hWrites);

			                const BlurPushConstants hPc{bounds.width, bounds.height, bounds.x, bounds.y, 1u, 0.0f, 0.0f, 0.0f};
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

			                gpu::CommandList cmd(ctx.recorder.GetCommandBuffer());
			                cmd.BindComputePipeline(blurPipeline.pipeline, blurPipeline.layout);

			                const VkDescriptorImageInfo vStorageInfo{
			                        .sampler = VK_NULL_HANDLE,
			                        .imageView = m_atlasManager.GetAtlasView(),
			                        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
			                };
			                const VkDescriptorImageInfo vSampledInfo{
			                        .sampler = m_blurSampler,
			                        .imageView = m_blurScratch.GetDefaultView(),
			                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			                };
			                const VkWriteDescriptorSet vWrites[]{
			                        {
			                                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                                .dstBinding = 0,
			                                .descriptorCount = 1,
			                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			                                .pImageInfo = &vStorageInfo,
			                        },
			                        {
			                                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			                                .dstBinding = 1,
			                                .descriptorCount = 1,
			                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			                                .pImageInfo = &vSampledInfo,
			                        },
			                };
			                cmd.PushDescriptorSet(gpu::PipelineBindPoint::Compute, blurPipeline.layout, 0, 2, vWrites);

			                const BlurPushConstants vPc{bounds.width, bounds.height, bounds.x, bounds.y, 0u, 0.0f, 0.0f, 0.0f};
			                cmd.PushConstantsRaw(blurPipeline.layout, gpu::ShaderStage::Compute, 0, std::as_bytes(std::span{&vPc, 1}));

			                cmd.Dispatch((bounds.width + 15u) / 16u, (bounds.height + 15u) / 16u, 1u);
		                });
	}
} // namespace aether
