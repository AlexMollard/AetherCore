#include "rendering/LocalShadowService.hpp"

#include <algorithm>
#include <cmath>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>

#include "camera/CameraManager.hpp"
#include "gpu/BindlessManager.hpp"
#include "io/FileSystem.hpp"
#include "passes/CullPass.hpp"
#include "rendering/Renderer.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/Scene.hpp"
#include "scene/World.hpp"
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
		std::uint32_t isHorizontal;
		float _pad0;
	};
	static_assert(sizeof(BlurPushConstants) == 16, "BlurPushConstants must be 16 bytes");

	void LocalShadowService::Initialize(VulkanContext& context, BindlessManager& bindless, const Swapchain& swapchain, const RenderQueueSharedPipelines& pipelines)
	{
		const VkDevice device = context.GetDevice().device;
		const VmaAllocator allocator = context.GetAllocator();

		m_atlasManager.Initialize(context, bindless);
		m_atlasBindlessSlot = m_atlasManager.GetBindlessSlot();

		m_shadowRenderQueue.Initialize(device, allocator, pipelines, 4096, 512, 0u);
		m_shadowRenderQueue.SetTracyVkCtx(context.GetTracyVkCtx());

		// Create the shadow depth pipeline (reads VP from per-light FrameConstants via BDA).
		const VkFormat depthFormat = swapchain.GetDepthFormat();
		AE_EXPECT_OR_THROW(pipeline,
		        GraphicsPipeline::Create(device,
		                {
		                        .shaderVfsPath = "shaders://local_shadow_depth.slang.spv",
		                        .colorFormat = VK_FORMAT_R32G32_SFLOAT,
		                        .depthFormat = depthFormat,
		                        .depthTestEnable = true,
		                        .depthWriteEnable = true,
		                        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		                }));
		m_shadowPipeline = std::move(pipeline);

		m_perLightShadows.reserve(kMaxLocalShadows);
		m_lightShadowIndices.reserve(4096);

		// Allocate GPU buffers for per-light shadow data and light constants.
		constexpr VkBufferUsageFlags kSsboBda = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

		AE_EXPECT_OR_THROW(sdBuf, UniqueBuffer::CreateMapped(allocator, device,
		        static_cast<VkDeviceSize>(kMaxLocalShadows) * sizeof(ShadowLightData), kSsboBda, "LocalShadow.ShadowData"));
		m_shadowDataBuffer = std::move(sdBuf);
		m_shadowDataAddr = m_shadowDataBuffer.GetDeviceAddress();

		AE_EXPECT_OR_THROW(lcBuf, UniqueBuffer::CreateMapped(allocator, device,
		        static_cast<VkDeviceSize>(kMaxLocalShadows) * sizeof(FrameConstants), kSsboBda, "LocalShadow.LightConstants"));
		m_lightConstantsBuffer = std::move(lcBuf);
		m_lightConstantsAddr = m_lightConstantsBuffer.GetDeviceAddress();

		// ── Create blur scratch image ──────────────────────────────────────
		AE_EXPECT_OR_THROW(scratchImg, UniqueImage::Create(device, allocator,
		        {
		                .extent = { ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight },
		                .format = ShadowAtlasManager::kAtlasFormat,
		                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		                .debugName = "ShadowBlurScratch",
		        }));
		m_blurScratch = std::move(scratchImg);

		// ── Create VSM blur compute pipeline ───────────────────────────────
		AE_EXPECT_OR_THROW(spirvBytes, io::FileSystem::ReadFile("shaders://vsm_blur.slang.spv"));
		AE_EXPECT_OR_THROW(blurModule, vkutil::CreateShaderModule(device, spirvBytes, "vsm_blur"));

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

			VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
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

			VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			plci.setLayoutCount = 1;
			plci.pSetLayouts = &m_blurDescriptorSetLayout;
			plci.pushConstantRangeCount = 1;
			plci.pPushConstantRanges = &pcRange;
			AE_ASSERT_ALWAYS(vkCreatePipelineLayout(device, &plci, nullptr, &m_blurPipelineLayout) == VK_SUCCESS, "Failed to create blur pipeline layout");
		}

		// Compute pipeline.
		{
			VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
			cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			cpci.stage.module = blurModule;
			cpci.stage.pName = "main";
			cpci.layout = m_blurPipelineLayout;
			AE_ASSERT_ALWAYS(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_blurPipeline) == VK_SUCCESS, "Failed to create blur compute pipeline");
		}

		vkDestroyShaderModule(device, blurModule, nullptr);

		// Sampler for blur input (nearest clamp-to-edge — texel fetch, sampler unused).
		{
			VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			sci.magFilter = VK_FILTER_NEAREST;
			sci.minFilter = VK_FILTER_NEAREST;
			sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			AE_ASSERT_ALWAYS(vkCreateSampler(device, &sci, nullptr, &m_blurSampler) == VK_SUCCESS, "Failed to create blur sampler");
		}

		// Descriptor pool.
		{
			VkDescriptorPoolSize poolSizes[2]{};
			poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			poolSizes[0].descriptorCount = 2;
			poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			poolSizes[1].descriptorCount = 2;

			VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			dpci.maxSets = 2;
			dpci.poolSizeCount = 2;
			dpci.pPoolSizes = poolSizes;
			AE_ASSERT_ALWAYS(vkCreateDescriptorPool(device, &dpci, nullptr, &m_blurDescriptorPool) == VK_SUCCESS, "Failed to create blur descriptor pool");
		}

		// Allocate two descriptor sets (H and V variants).
		{
			VkDescriptorSetLayout layouts[2] = { m_blurDescriptorSetLayout, m_blurDescriptorSetLayout };
			VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			dsai.descriptorPool = m_blurDescriptorPool;
			dsai.descriptorSetCount = 2;
			dsai.pSetLayouts = layouts;
			VkDescriptorSet sets[2];
			AE_ASSERT_ALWAYS(vkAllocateDescriptorSets(device, &dsai, sets) == VK_SUCCESS, "Failed to allocate blur descriptor sets");
			m_blurDescriptorSetH = sets[0];
			m_blurDescriptorSetV = sets[1];
		}

		// Write descriptor bindings once — the atlas and scratch images never change.
		/* DescriptorSetH: binding 0 = scratch (storage), binding 1 = atlas (sampled). */
		{
			VkDescriptorImageInfo scratchOut{};
			scratchOut.imageView = m_blurScratch.GetDefaultView();
			scratchOut.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

			VkDescriptorImageInfo atlasIn{};
			atlasIn.sampler = m_blurSampler;
			atlasIn.imageView = m_atlasManager.GetAtlasImage().GetDefaultView();
			atlasIn.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkWriteDescriptorSet writes[2]{};
			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = m_blurDescriptorSetH;
			writes[0].dstBinding = 0;
			writes[0].descriptorCount = 1;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writes[0].pImageInfo = &scratchOut;

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = m_blurDescriptorSetH;
			writes[1].dstBinding = 1;
			writes[1].descriptorCount = 1;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[1].pImageInfo = &atlasIn;

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}

		/* DescriptorSetV: binding 0 = atlas (storage), binding 1 = scratch (sampled). */
		{
			VkDescriptorImageInfo atlasOut{};
			atlasOut.imageView = m_atlasManager.GetAtlasImage().GetDefaultView();
			atlasOut.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

			VkDescriptorImageInfo scratchIn{};
			scratchIn.sampler = m_blurSampler;
			scratchIn.imageView = m_blurScratch.GetDefaultView();
			scratchIn.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkWriteDescriptorSet writes[2]{};
			writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[0].dstSet = m_blurDescriptorSetV;
			writes[0].dstBinding = 0;
			writes[0].descriptorCount = 1;
			writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writes[0].pImageInfo = &atlasOut;

			writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[1].dstSet = m_blurDescriptorSetV;
			writes[1].dstBinding = 1;
			writes[1].descriptorCount = 1;
			writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[1].pImageInfo = &scratchIn;

			vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
		}
	}

	void LocalShadowService::Shutdown(VkDevice device)
	{
		m_shadowRenderQueue.Shutdown();
		m_shadowPipeline.Destroy();
		m_shadowDataBuffer.Reset();
		m_lightConstantsBuffer.Reset();
		m_atlasManager.Shutdown();

		m_blurScratch.Reset();

		if (m_blurSampler != VK_NULL_HANDLE)
		{
			vkDestroySampler(device, m_blurSampler, nullptr);
			m_blurSampler = VK_NULL_HANDLE;
		}
		if (m_blurDescriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, m_blurDescriptorPool, nullptr);
			m_blurDescriptorPool = VK_NULL_HANDLE;
		}
		if (m_blurPipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, m_blurPipeline, nullptr);
			m_blurPipeline = VK_NULL_HANDLE;
		}
		if (m_blurPipelineLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, m_blurPipelineLayout, nullptr);
			m_blurPipelineLayout = VK_NULL_HANDLE;
		}
		if (m_blurDescriptorSetLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, m_blurDescriptorSetLayout, nullptr);
			m_blurDescriptorSetLayout = VK_NULL_HANDLE;
		}
	}

	void LocalShadowService::PrepareQueues(const std::uint32_t drawSlot, Scene& scene, World& world)
	{
		m_shadowRenderQueue.SetWriteSlot(drawSlot);
		m_shadowRenderQueue.Clear(drawSlot);
		WorldRenderer::Flush(scene, m_shadowRenderQueue);
		WorldRenderer::Flush(world, m_shadowRenderQueue);
	}

	void LocalShadowService::BuildFrameShadowData(const RenderFramePacket& packet, const std::uint32_t frameIdx, CameraManager& cameraManager, const Renderer& renderer, Scene& scene, World& world, FrameConstants& fc)
	{
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

		const std::uint32_t pointCount = static_cast<std::uint32_t>(renderer.GetPointLights().size());
		const std::uint32_t spotCount = static_cast<std::uint32_t>(renderer.GetSpotLights().size());

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
			std::span<const Renderer::PointLight> ptLights = renderer.GetPointLights();
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
			std::span<const Renderer::SpotLight> spLights = renderer.GetSpotLights();
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
		std::sort(candidates.begin(), candidates.end(),
		        [](const ShadowCandidate& a, const ShadowCandidate& b)
		        {
			        return a.distanceSq < b.distanceSq;
		        });

		// Clamp to budget.
		const std::uint32_t budget = std::min(static_cast<std::uint32_t>(candidates.size()), kMaxLocalShadows);

		// Allocate atlas regions and build per-light data.
		for (std::uint32_t i = 0; i < budget; ++i)
		{
			const ShadowCandidate& c = candidates[i];

			// Determine region size based on distance.
			const float dist = std::sqrt(c.distanceSq);
			std::uint32_t shadowRes = 256u;
			if (dist < 15.0f)
			{
				shadowRes = 512u;
			}
			else if (dist > 60.0f)
			{
				shadowRes = 128u;
			}

			if (c.lightType == 0u)
			{
				// Point light: two perspective faces (front+back), each half resolution.
				// Both use lightType=1 on the GPU side (2 consecutive entries).
				const std::uint32_t faceRes = std::max(shadowRes / 2u, 64u);

				ShadowAtlasManager::Region r0 = m_atlasManager.Allocate(faceRes, faceRes);
				ShadowAtlasManager::Region r1 = m_atlasManager.Allocate(faceRes, faceRes);
				if (!r0.IsValid() || !r1.IsValid())
				{
					break;
				}

				// Front face: lookAt from light toward camera.
				{
					const glm::vec3 lightToCam = glm::normalize(camPos - c.position);
					const glm::mat4 lightView = glm::lookAt(c.position, c.position + lightToCam, glm::vec3(0.0f, 1.0f, 0.0f));
					const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(glm::radians(90.0f), 1.0f, 1.0f, 0.1f, c.radius);

					m_perLightShadows.push_back(PerLightShadow{
					        .viewProj = lightProj * lightView,
					        .region = r0,
					        .depthBias = 0.002f,
					        .lightType = 1u, // point
					});
				}

				// Back face: lookAt from camera toward light.
				{
					const glm::vec3 camToLight = glm::normalize(c.position - camPos);
					const glm::mat4 lightView = glm::lookAt(c.position, c.position + camToLight, glm::vec3(0.0f, 1.0f, 0.0f));
					const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(glm::radians(90.0f), 1.0f, 1.0f, 0.1f, c.radius);

					m_perLightShadows.push_back(PerLightShadow{
					        .viewProj = lightProj * lightView,
					        .region = r1,
					        .depthBias = 0.002f,
					        .lightType = 1u, // point (same type, 2nd entry)
					});
				}
			}
			else
			{
				// Spot light: single perspective region.
				ShadowAtlasManager::Region r = m_atlasManager.Allocate(shadowRes, shadowRes);
				if (!r.IsValid())
				{
					break;
				}

				std::span<const Renderer::SpotLight> spLights = renderer.GetSpotLights();
				const Renderer::SpotLight& src = spLights[c.lightIndex];
				const glm::vec3 lightDir = glm::normalize(src.direction);
				const glm::vec3 up = (std::abs(glm::dot(lightDir, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
				                           ? glm::vec3(1.0f, 0.0f, 0.0f)
				                           : glm::vec3(0.0f, 1.0f, 0.0f);
				const glm::mat4 lightView = glm::lookAt(src.position, src.position + lightDir, up);
				const float fov = 2.0f * src.outerAngleRad;
				const glm::mat4 lightProj = glm::perspectiveFovRH_ZO(fov, 1.0f, 1.0f, 0.1f, src.radius);

				m_perLightShadows.push_back(PerLightShadow{
				        .viewProj = lightProj * lightView,
				        .region = r,
				        .depthBias = 0.001f,
				        .lightType = 0u, // spot
				});
			}
		}

		// Write ShadowLightData to GPU buffer.
		const std::uint32_t shadowCount = static_cast<std::uint32_t>(m_perLightShadows.size());
		ShadowLightData* mapped = static_cast<ShadowLightData*>(m_shadowDataBuffer.GetAllocationInfo().pMappedData);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			const PerLightShadow& pls = m_perLightShadows[i];
			mapped[i].viewProj = pls.viewProj;
			mapped[i].atlasRegion = glm::vec4(
			        static_cast<float>(pls.region.x) / static_cast<float>(ShadowAtlasManager::kAtlasWidth),
			        static_cast<float>(pls.region.y) / static_cast<float>(ShadowAtlasManager::kAtlasHeight),
			        static_cast<float>(pls.region.width) / static_cast<float>(ShadowAtlasManager::kAtlasWidth),
			        static_cast<float>(pls.region.height) / static_cast<float>(ShadowAtlasManager::kAtlasHeight));
			mapped[i].depthBias = pls.depthBias;
			mapped[i].lightType = pls.lightType;
		}
		AE_EXPECT_OR_THROW_VOID(m_shadowDataBuffer.FlushMapped());

		// Write per-light frame constants (just viewProj) for atlas rendering.
		FrameConstants* lightFc = static_cast<FrameConstants*>(m_lightConstantsBuffer.GetAllocationInfo().pMappedData);
		for (std::uint32_t i = 0; i < shadowCount; ++i)
		{
			lightFc[i].viewProj = m_perLightShadows[i].viewProj;
			lightFc[i].cameraWorldPos = glm::vec4(camPos, 1.0f);
		}
		AE_EXPECT_OR_THROW_VOID(m_lightConstantsBuffer.FlushMapped());

		// Build per-light shadow index mapping for all lights in GpuLight buffer order.
		// Point lights = first pointCount slots, spot lights = next spotCount slots.
		const std::uint32_t totalLights = pointCount + spotCount;
		m_lightShadowIndices.assign(totalLights, glm::vec2(-1.0f, 1.0f));
		for (std::uint32_t i = 0; i < budget; ++i)
		{
			const ShadowCandidate& c = candidates[i];
			float shadowIdx = static_cast<float>(i);
			// Candidate type 0=point, 1=spot on CPU side. Point lights get the
			// shadowIdx pointing to the first of 2 consecutive ShadowLightData entries.
			if (c.lightType == 0u)
			{
				// Point light: stored in first pointCount GpuLight slots.
				m_lightShadowIndices[c.lightIndex] = glm::vec2(shadowIdx, 1.0f);
			}
			else
			{
				// Spot light: stored after all point lights.
				m_lightShadowIndices[pointCount + c.lightIndex] = glm::vec2(shadowIdx, 1.0f);
			}
		}

		// Fill FrameConstants for the shader.
		fc.shadowAtlasSlot = m_atlasBindlessSlot;
		fc.shadowLightCount = shadowCount;
		fc.shadowLightDataAddr = m_shadowDataAddr;
	}

	void LocalShadowService::RegisterPasses(RenderGraph& graph, BindlessManager& bindless, VkDevice device, CullPass& cullPass, VkFormat depthFormat)
	{
		// Register the atlas as an external image in the render graph.
		m_atlasImage = graph.RegisterImage(m_atlasManager.GetAtlasImage().Get(), m_atlasManager.GetAtlasView(), VK_IMAGE_ASPECT_COLOR_BIT);

		// Register the blur scratch image.
		m_blurScratchImage = graph.RegisterImage(m_blurScratch.Get(), m_blurScratch.GetDefaultView(), VK_IMAGE_ASPECT_COLOR_BIT);

		// Create a transient depth attachment for the atlas render pass.
		RGImage atlasDepth = graph.CreateTransientDepth(depthFormat,
		        VkExtent2D{ ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight },
		        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

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
			                m_shadowRenderQueue.PrepareAndDispatch(ctx.recorder.GetCommandBuffer(), ctx.frameConstantsAddr, cullPass.GetPipeline(), cullPass.GetPipelineLayout(), ctx.frameIndex);
		                });

		// Graphics pass: render all shadow casters into the atlas with per-light scissoring.
		graph.AddPass("$LocalShadowAtlasRender")
		        .WriteColor(m_atlasImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, ClearColorValue(1.0f, 1.0f, 1.0f, 1.0f))
		        .WriteDepth(atlasDepth, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, ClearDepthValue(1.0f))
		        .SetExtent(VkExtent2D{ ShadowAtlasManager::kAtlasWidth, ShadowAtlasManager::kAtlasHeight })
		        .Execute(
		                [this](PassContext& ctx)
		                {
			                if (m_perLightShadows.empty())
			                {
				                return;
			                }

			                for (std::uint32_t li = 0; li < static_cast<std::uint32_t>(m_perLightShadows.size()); ++li)
			                {
				                const PerLightShadow& pls = m_perLightShadows[li];

				                const VkViewport vp{
					                .x = static_cast<float>(pls.region.x),
					                .y = static_cast<float>(pls.region.y),
					                .width = static_cast<float>(pls.region.width),
					                .height = static_cast<float>(pls.region.height),
					                .minDepth = 0.0f,
					                .maxDepth = 1.0f,
				                };
				                vkCmdSetViewport(ctx.recorder.GetCommandBuffer(), 0, 1, &vp);

				                const VkRect2D scissor{
					                .offset = { static_cast<std::int32_t>(pls.region.x), static_cast<std::int32_t>(pls.region.y) },
					                .extent = { pls.region.width, pls.region.height },
				                };
				                vkCmdSetScissor(ctx.recorder.GetCommandBuffer(), 0, 1, &scissor);

				                const VkDeviceAddress lightFcAddr = m_lightConstantsAddr + static_cast<VkDeviceSize>(li) * sizeof(FrameConstants);
				                m_shadowRenderQueue.FlushDrawWithFrameAddr(ctx.recorder, VK_NULL_HANDLE, VK_NULL_HANDLE, lightFcAddr, &m_shadowPipeline);
			                }

			                m_shadowRenderQueue.Clear(ctx.frameIndex % RenderQueue::kFramesInFlight);
		                });

		// ── VSM blur passes ────────────────────────────────────────────────
		// Horizontal blur: read atlas (sampled), write scratch (storage).
		// Descriptors set up once in Initialize() — no per-frame updates needed.
		graph.AddComputePass("$VSMBlurH")
		        .ReadTexture(m_atlasImage)
		        .WriteStorageImage(m_blurScratchImage)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                constexpr std::uint32_t kW = ShadowAtlasManager::kAtlasWidth;
			                constexpr std::uint32_t kH = ShadowAtlasManager::kAtlasHeight;

			                vkCmdBindPipeline(ctx.recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipeline);
			                vkCmdBindDescriptorSets(ctx.recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipelineLayout, 0, 1, &m_blurDescriptorSetH, 0, nullptr);

			                const BlurPushConstants hPc{ kW, kH, 1u, 0.0f };
			                vkCmdPushConstants(ctx.recorder.GetCommandBuffer(), m_blurPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPushConstants), &hPc);

			                vkCmdDispatch(ctx.recorder.GetCommandBuffer(), (kW + 15u) / 16u, (kH + 15u) / 16u, 1u);
		                });

		// Vertical blur: read scratch (sampled), write atlas (storage).
		graph.AddComputePass("$VSMBlurV")
		        .ReadTexture(m_blurScratchImage)
		        .WriteStorageImage(m_atlasImage)
		        .ExecuteCompute(
		                [this](PassContext& ctx)
		                {
			                constexpr std::uint32_t kW = ShadowAtlasManager::kAtlasWidth;
			                constexpr std::uint32_t kH = ShadowAtlasManager::kAtlasHeight;

			                vkCmdBindPipeline(ctx.recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipeline);
			                vkCmdBindDescriptorSets(ctx.recorder.GetCommandBuffer(), VK_PIPELINE_BIND_POINT_COMPUTE, m_blurPipelineLayout, 0, 1, &m_blurDescriptorSetV, 0, nullptr);

			                const BlurPushConstants vPc{ kW, kH, 0u, 0.0f };
			                vkCmdPushConstants(ctx.recorder.GetCommandBuffer(), m_blurPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPushConstants), &vPc);

			                vkCmdDispatch(ctx.recorder.GetCommandBuffer(), (kW + 15u) / 16u, (kH + 15u) / 16u, 1u);
		                });
	}
} // namespace aether
