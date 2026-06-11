#include "rendering/LightingManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/common.hpp>
#include <vector>

#include "gpu/DescriptorSetLayoutOps.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "io/FileSystem.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "utils/Expected.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ShaderUtils.hpp"

namespace aether
{
	struct LightingComputePush
	{
		glm::mat4 viewProj{1.0f};
		glm::vec4 params0{0.0f}; // x=nearClip, y=pixelScaleY, z=screenW, w=screenH
		glm::uvec4 params1{0u};  // x=tilePx, y=tilesX, z=tilesY, w=lightCount
		glm::uvec4 params2{0u};  // x=maxLightsPerTile
	};

	void LightingManager::Initialize(GpuDevice& device, const VulkanContext& context)
	{
		AE_PROFILE_ZONE();
		m_device = &device;
		m_context = &context;
		m_renderer = nullptr;

		const gpu::GpuDescriptorSetLayoutBinding bindings[] = {
		        {
		                .binding = 0,
		                .descriptorType = gpu::DescriptorType::StorageBuffer,
		                .descriptorCount = 1,
		                .stageFlags = gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute,
		        },
		        {
		                .binding = 1,
		                .descriptorType = gpu::DescriptorType::StorageBuffer,
		                .descriptorCount = 1,
		                .stageFlags = gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute,
		        },
		        {
		                .binding = 2,
		                .descriptorType = gpu::DescriptorType::StorageBuffer,
		                .descriptorCount = 1,
		                .stageFlags = gpu::ShaderStage::Fragment | gpu::ShaderStage::Compute,
		        },
		};

		const gpu::DescriptorSetLayoutDesc layoutDesc{
		        .bindings = std::span<const gpu::GpuDescriptorSetLayoutBinding>(bindings, std::size(bindings)),
		        .flags = gpu::DescriptorSetLayoutFlags::PushDescriptor,
		};

		auto result = gpu::CreateDescriptorSetLayout(device, layoutDesc);
		if (!result)
		{
			Throw(AetherError::Vulkan(0, "LightingManager: failed to create lighting descriptor set layout."));
		}
		m_setLayout = *result;

		for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			EnsureBuffers(i, 1, 1, 1);
		}
	}

	void LightingManager::LinkRenderer(const Renderer& renderer)
	{
		m_renderer = &renderer;
	}

	void LightingManager::Shutdown()
	{
		AE_PROFILE_ZONE();
		if (!m_context)
		{
			return;
		}

		const VkDevice device = m_context->GetDevice().device;
		for (auto& frame: m_buffers)
		{
			frame.lights.Reset();
			frame.tileHeaders.Reset();
			frame.tileIndices.Reset();
			frame.lightsCapacity = 0;
			frame.headersCapacity = 0;
			frame.indicesCapacity = 0;
		}

		if (m_initPipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, m_initPipeline, nullptr);
			m_initPipeline = VK_NULL_HANDLE;
		}
		if (m_cullPipeline != VK_NULL_HANDLE)
		{
			vkDestroyPipeline(device, m_cullPipeline, nullptr);
			m_cullPipeline = VK_NULL_HANDLE;
		}
		if (m_computeLayout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, m_computeLayout, nullptr);
			m_computeLayout = VK_NULL_HANDLE;
		}
		if (m_setLayout != nullptr)
		{
			gpu::DestroyDescriptorSetLayout(*m_device, m_setLayout);
			m_setLayout = nullptr;
		}

		m_context = nullptr;
		m_renderer = nullptr;
		m_device = nullptr;
	}

	gpu::DescriptorSetLayout LightingManager::GetSetLayout() const
	{
		return m_setLayout;
	}

	void LightingManager::PushLightingDescriptor(gpu::CommandList& cmd, void* layout, const std::uint32_t frameSlot) const
	{
		auto& frame = m_buffers[frameSlot];
		const gpu::GpuDescriptorBufferInfo lightInfo{
		        .buffer = frame.lights.Get(),
		        .offset = 0,
		        .range = frame.lights.GetSize(),
		};
		const gpu::GpuDescriptorBufferInfo headerInfo{
		        .buffer = frame.tileHeaders.Get(),
		        .offset = 0,
		        .range = frame.tileHeaders.GetSize(),
		};
		const gpu::GpuDescriptorBufferInfo indexInfo{
		        .buffer = frame.tileIndices.Get(),
		        .offset = 0,
		        .range = frame.tileIndices.GetSize(),
		};
		const gpu::GpuWriteDescriptorSet writes[] = {
		        {
		                .dstBinding = 0,
		                .descriptorCount = 1,
		                .descriptorType = gpu::DescriptorType::StorageBuffer,
		                .bufferInfo = &lightInfo,
		        },
		        {
		                .dstBinding = 1,
		                .descriptorCount = 1,
		                .descriptorType = gpu::DescriptorType::StorageBuffer,
		                .bufferInfo = &headerInfo,
		        },
		        {
		                .dstBinding = 2,
		                .descriptorCount = 1,
		                .descriptorType = gpu::DescriptorType::StorageBuffer,
		                .bufferInfo = &indexInfo,
		        },
		};
		cmd.PushDescriptorSet(layout, 1, std::span<const gpu::GpuWriteDescriptorSet>(writes, std::size(writes)));
	}

	void LightingManager::UpdateForView(const std::uint32_t frameSlot,
	        gpu::CommandList& cmd,
	        const Camera& camera,
	        const GpuExtent2D extent,
	        FrameConstants& fc,
	        const bool enableBinningForView,
	        const bool isAsyncCompute,
	        const std::span<const Renderer::PointLight> pointLights,
	        const std::span<const Renderer::SpotLight> spotLights) const
	{
		AE_PROFILE_ZONE();
		if (!enableBinningForView || extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
			return;
		}

		if (m_gpuBinningEnabled && cmd.IsValid())
		{
			UpdateForViewGpu(frameSlot, cmd, camera, extent, fc, pointLights, spotLights);
			if (!isAsyncCompute)
			{
				// Same queue: explicit compute→fragment barrier required.
				// Async path: the semaphore wait at DRAW_INDIRECT in SubmitAndPresent covers this.
				cmd.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::FragmentShader, gpu::AccessFlags::ShaderStorageRead);
			}
			return;
		}

		UpdateForViewCpu(frameSlot, camera, extent, fc, pointLights, spotLights);
	}

	void LightingManager::BuildLightList(std::vector<GpuLight>& outLights, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights)
	{
		outLights.reserve(outLights.size() + pointLights.size() + spotLights.size());

		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pointLights.size()); ++i)
		{
			const Renderer::PointLight& src = pointLights[i];
			outLights.push_back(GpuLight{
			        .positionRadius = glm::vec4(src.position, src.radius),
			        .colorIntensity = glm::vec4(src.color, src.intensity),
			        .directionType = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
			        .params = glm::vec4(0.0f),
			        .shadowIndex = glm::vec4(-1.0f, 1.0f, 0.0f, 0.0f),
			});
		}
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(spotLights.size()); ++i)
		{
			const Renderer::SpotLight& src = spotLights[i];
			outLights.push_back(GpuLight{
			        .positionRadius = glm::vec4(src.position, src.radius),
			        .colorIntensity = glm::vec4(src.color, src.intensity),
			        .directionType = glm::vec4(glm::normalize(src.direction), 1.0f),
			        .params = glm::vec4(std::cos(src.innerAngleRad), std::cos(src.outerAngleRad), 0.0f, 0.0f),
			        .shadowIndex = glm::vec4(-1.0f, 1.0f, 0.0f, 0.0f),
			});
		}
	}

	void LightingManager::UpdateForViewGpu(
	        const std::uint32_t frameSlot, gpu::CommandList& cmd, const Camera& camera, const GpuExtent2D extent, FrameConstants& fc, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights)
	        const
	{
		AE_PROFILE_ZONE();
		std::vector<GpuLight> lights;
		BuildLightList(lights, pointLights, spotLights);

		const std::uint32_t tilesX = (extent.width + kTileSizePx - 1u) / kTileSizePx;
		const std::uint32_t tilesY = (extent.height + kTileSizePx - 1u) / kTileSizePx;
		const std::size_t tileCount = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);
		const std::size_t indexCount = tileCount * static_cast<std::size_t>(m_maxLightsPerTile);

		EnsureBuffers(frameSlot, lights.size(), tileCount, indexCount);
		auto& frame = m_buffers[frameSlot];
		if (!lights.empty())
		{
			std::memcpy(frame.lights.GetAllocationInfo().pMappedData, lights.data(), lights.size() * sizeof(GpuLight));
		}
		AE_EXPECT_OR_THROW_VOID(frame.lights.FlushMapped());

		EnsureComputePipeline();

		const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		const glm::mat4 proj = camera.GetProjectionMatrix(aspect);
		LightingComputePush push{};
		push.viewProj = proj * camera.GetViewMatrix();
		push.params0 = glm::vec4(camera.GetNearPlane(), 0.5f * static_cast<float>(extent.height) * std::abs(proj[1][1]), static_cast<float>(extent.width), static_cast<float>(extent.height));
		push.params1 = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		push.params2 = glm::uvec4(m_maxLightsPerTile, 0u, 0u, 0u);

		cmd.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

		// Bind the InitTiles compute pipeline FIRST so CommandList's cached
		// bind point (used by the cached PushDescriptorSet overload) is
		// Compute. This ordering also satisfies the Vulkan spec's
		// pipeline-layout compatibility check for push descriptors: the
		// bound pipeline layout must be compatible with the layout passed
		// to vkCmdPushDescriptorSetKHR. Both are m_computeLayout here.
		cmd.BeginDebugLabel("LightCull.InitTiles", 0.9f, 0.65f, 0.1f);
		cmd.BindComputePipeline(m_initPipeline, m_computeLayout);
		{
			const auto& buf = m_buffers[frameSlot];
			const gpu::GpuDescriptorBufferInfo lightInfo{
			        .buffer = buf.lights.Get(),
			        .offset = 0,
			        .range = buf.lights.GetSize(),
			};
			const gpu::GpuDescriptorBufferInfo headerInfo{
			        .buffer = buf.tileHeaders.Get(),
			        .offset = 0,
			        .range = buf.tileHeaders.GetSize(),
			};
			const gpu::GpuDescriptorBufferInfo indexInfo{
			        .buffer = buf.tileIndices.Get(),
			        .offset = 0,
			        .range = buf.tileIndices.GetSize(),
			};
			const std::array<gpu::GpuWriteDescriptorSet, 3> writes{
			        gpu::GpuWriteDescriptorSet{
			                .dstBinding = 0,
			                .descriptorCount = 1,
			                .descriptorType = gpu::DescriptorType::StorageBuffer,
			                .bufferInfo = &lightInfo,
			        },
			        gpu::GpuWriteDescriptorSet{
			                .dstBinding = 1,
			                .descriptorCount = 1,
			                .descriptorType = gpu::DescriptorType::StorageBuffer,
			                .bufferInfo = &headerInfo,
			        },
			        gpu::GpuWriteDescriptorSet{
			                .dstBinding = 2,
			                .descriptorCount = 1,
			                .descriptorType = gpu::DescriptorType::StorageBuffer,
			                .bufferInfo = &indexInfo,
			        },
			};
			cmd.PushDescriptorSet(m_computeLayout, 0, std::span<const gpu::GpuWriteDescriptorSet>(writes));
		}
		{
			cmd.PushConstantsRaw(m_computeLayout, gpu::ShaderStage::Compute, 0, gpu::AsPushConstantBytes(push));
		}
		const std::uint32_t tileGroups = static_cast<std::uint32_t>((tileCount + 63u) / 64u);
		cmd.Dispatch(tileGroups, 1, 1);
		cmd.EndDebugLabel();

		cmd.PipelineMemoryBarrier(gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

		cmd.BeginDebugLabel("LightCull.BinLights", 0.9f, 0.3f, 0.1f);
		cmd.BindComputePipeline(m_cullPipeline, m_computeLayout);
		const std::uint32_t lightGroups = static_cast<std::uint32_t>((lights.size() + 63u) / 64u);
		if (lightGroups > 0u)
		{
			cmd.Dispatch(lightGroups, 1, 1);
		}
		cmd.EndDebugLabel();

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, m_maxLightsPerTile);
	}

	void LightingManager::EmitAcquireBarriers(const std::uint32_t /*frameSlot*/, gpu::CommandList& /*graphicsCmd*/, const std::uint32_t /*srcFamily*/, const std::uint32_t /*dstFamily*/) const
	{
		// maintenance9 eliminates queue family ownership transfers entirely.
	}

	void LightingManager::UpdateForViewCpu(
	        const std::uint32_t frameSlot, const Camera& camera, const GpuExtent2D extent, FrameConstants& fc, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights) const
	{
		AE_PROFILE_ZONE();
		std::vector<GpuLight> lights;
		BuildLightList(lights, pointLights, spotLights);

		const std::uint32_t tilesX = (extent.width + kTileSizePx - 1u) / kTileSizePx;
		const std::uint32_t tilesY = (extent.height + kTileSizePx - 1u) / kTileSizePx;
		const std::size_t tileCount = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);
		std::vector<TileHeader> headers(tileCount);
		std::vector<std::uint32_t> counts(tileCount, 0u);

		struct ScreenBounds
		{
			int minTx = 0;
			int maxTx = -1;
			int minTy = 0;
			int maxTy = -1;
			bool visible = false;
		};

		std::vector<ScreenBounds> bounds(lights.size());

		const glm::mat4 view = camera.GetViewMatrix();
		const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
		const glm::mat4 proj = camera.GetProjectionMatrix(aspect);
		const glm::mat4 viewProj = proj * view;
		const float pixelScaleY = 0.5f * static_cast<float>(extent.height) * std::abs(proj[1][1]);
		const float nearClip = camera.GetNearPlane();

		auto computeLightBounds = [&](const GpuLight& light, ScreenBounds& out)
		{
			const glm::vec4 viewPos4 = view * glm::vec4(light.positionRadius.x, light.positionRadius.y, light.positionRadius.z, 1.0f);
			const float depth = -viewPos4.z;
			if (depth < nearClip)
			{
				out.visible = false;
				return;
			}

			const glm::vec4 clip = viewProj * glm::vec4(light.positionRadius.x, light.positionRadius.y, light.positionRadius.z, 1.0f);
			if (std::abs(clip.w) <= 1e-6f)
			{
				out.visible = false;
				return;
			}

			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			const float screenX = (ndc.x * 0.5f + 0.5f) * static_cast<float>(extent.width);
			const float screenY = (ndc.y * 0.5f + 0.5f) * static_cast<float>(extent.height);
			const float r = light.positionRadius.w;
			const float effectiveDepth = std::max(std::sqrt(std::max(depth * depth - r * r, 0.0f)), nearClip);
			const float radiusPx = r * pixelScaleY / effectiveDepth;
			if (radiusPx <= 0.5f)
			{
				out.visible = false;
				return;
			}

			const float minX = screenX - radiusPx;
			const float maxX = screenX + radiusPx;
			const float minY = screenY - radiusPx;
			const float maxY = screenY + radiusPx;

			if (maxX < 0.0f || maxY < 0.0f || minX >= static_cast<float>(extent.width) || minY >= static_cast<float>(extent.height))
			{
				out.visible = false;
				return;
			}

			out.minTx = static_cast<int>(glm::clamp(std::floor(minX / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesX - 1u)));
			out.maxTx = static_cast<int>(glm::clamp(std::floor(maxX / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesX - 1u)));
			out.minTy = static_cast<int>(glm::clamp(std::floor(minY / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesY - 1u)));
			out.maxTy = static_cast<int>(glm::clamp(std::floor(maxY / static_cast<float>(kTileSizePx)), 0.0f, static_cast<float>(tilesY - 1u)));
			out.visible = true;
		};

		for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
		{
			computeLightBounds(lights[lightIndex], bounds[lightIndex]);
			if (!bounds[lightIndex].visible)
			{
				continue;
			}
			for (int ty = bounds[lightIndex].minTy; ty <= bounds[lightIndex].maxTy; ++ty)
			{
				for (int tx = bounds[lightIndex].minTx; tx <= bounds[lightIndex].maxTx; ++tx)
				{
					const std::size_t tile = static_cast<std::size_t>(ty) * tilesX + static_cast<std::size_t>(tx);
					++counts[tile];
				}
			}
		}

		std::uint32_t totalIndices = 0;
		for (std::size_t tile = 0; tile < tileCount; ++tile)
		{
			headers[tile].offset = totalIndices;
			headers[tile].count = counts[tile];
			totalIndices += counts[tile];
		}

		std::vector<std::uint32_t> indices(totalIndices);
		std::vector<std::uint32_t> cursors(tileCount, 0u);
		for (std::size_t tile = 0; tile < tileCount; ++tile)
		{
			cursors[tile] = headers[tile].offset;
		}

		for (std::size_t lightIndex = 0; lightIndex < lights.size(); ++lightIndex)
		{
			if (!bounds[lightIndex].visible)
			{
				continue;
			}
			for (int ty = bounds[lightIndex].minTy; ty <= bounds[lightIndex].maxTy; ++ty)
			{
				for (int tx = bounds[lightIndex].minTx; tx <= bounds[lightIndex].maxTx; ++tx)
				{
					const std::size_t tile = static_cast<std::size_t>(ty) * tilesX + static_cast<std::size_t>(tx);
					indices[cursors[tile]++] = static_cast<std::uint32_t>(lightIndex);
				}
			}
		}

		EnsureBuffers(frameSlot, lights.size(), headers.size(), indices.size());
		auto& frame = m_buffers[frameSlot];
		if (!lights.empty())
		{
			std::memcpy(frame.lights.GetAllocationInfo().pMappedData, lights.data(), lights.size() * sizeof(GpuLight));
		}
		if (!headers.empty())
		{
			std::memcpy(frame.tileHeaders.GetAllocationInfo().pMappedData, headers.data(), headers.size() * sizeof(TileHeader));
		}
		if (!indices.empty())
		{
			std::memcpy(frame.tileIndices.GetAllocationInfo().pMappedData, indices.data(), indices.size() * sizeof(std::uint32_t));
		}
		AE_EXPECT_OR_THROW_VOID(frame.lights.FlushMapped());
		AE_EXPECT_OR_THROW_VOID(frame.tileHeaders.FlushMapped());
		AE_EXPECT_OR_THROW_VOID(frame.tileIndices.FlushMapped());

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, m_maxLightsPerTile);
	}

	void LightingManager::EnsureBuffers(const std::uint32_t frameSlot, const std::size_t lightCount, const std::size_t tileCount, const std::size_t indexCount) const
	{
		AE_PROFILE_ZONE();
		auto& frame = m_buffers[frameSlot];
		const VkDevice device = m_context->GetDevice().device;
		const VmaAllocator allocator = m_context->GetAllocator();

		// Retire stale buffers from kMaxFramesInFlight frames ago - this slot is
		// guaranteed to have completed all GPU work referencing them.
		frame.staleBuffers.clear();

		auto ensureBuffer = [&](UniqueBuffer& buffer, std::size_t& capacity, const std::size_t required, const VkDeviceSize stride)
		{
			const std::size_t safeRequired = std::max<std::size_t>(required, 1u);
			if (buffer && capacity >= safeRequired)
			{
				return;
			}

			capacity = std::max(safeRequired, capacity * 2u);
			if (capacity == 0)
			{
				capacity = safeRequired;
			}

			VkBufferCreateInfo info{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			        .size = stride * capacity,
			        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			};
			VmaAllocationCreateInfo allocInfo{};
			allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
			allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
			// Create new buffer BEFORE releasing the old one - avoids use-after-free on creation failure.
			AE_EXPECT_OR_THROW(newBuf, UniqueBuffer::Create(allocator, device, info, allocInfo));
			// Defer destruction of the old buffer to this slot's next reuse cycle,
			// ensuring any in-flight GPU work referencing it has completed.
			if (buffer)
			{
				frame.staleBuffers.push_back(std::move(buffer));
			}
			buffer = std::move(newBuf);
		};

		ensureBuffer(frame.lights, frame.lightsCapacity, lightCount, sizeof(GpuLight));
		ensureBuffer(frame.tileHeaders, frame.headersCapacity, tileCount, sizeof(TileHeader));
		ensureBuffer(frame.tileIndices, frame.indicesCapacity, indexCount, sizeof(std::uint32_t));
	}

	void LightingManager::EnsureComputePipeline() const
	{
		AE_PROFILE_ZONE();
		if (m_computeLayout != VK_NULL_HANDLE && m_initPipeline != VK_NULL_HANDLE && m_cullPipeline != VK_NULL_HANDLE)
		{
			return;
		}

		const VkDevice device = m_context->GetDevice().device;
		AE_EXPECT_OR_THROW(spirv, io::FileSystem::ReadFile("shaders://tiled_light_cull.spv"));

		AE_EXPECT_OR_THROW(shaderModule, vkutil::CreateShaderModule(device, spirv, "LightingManager"));

		const VkPushConstantRange pushRange{
		        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		        .offset = 0,
		        .size = static_cast<std::uint32_t>(sizeof(LightingComputePush)),
		};
		const VkDescriptorSetLayout setLayoutHandle = static_cast<VkDescriptorSetLayout>(m_setLayout);
		const VkPipelineLayoutCreateInfo layoutInfo{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		        .setLayoutCount = 1,
		        .pSetLayouts = &setLayoutHandle,
		        .pushConstantRangeCount = 1,
		        .pPushConstantRanges = &pushRange,
		};
		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &m_computeLayout) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			Throw(AetherError::Vulkan(0, "LightingManager: failed to create compute pipeline layout."));
		}

		const VkPipelineShaderStageCreateInfo initStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule,
		        .pName = "initTiles",
		};
		const VkComputePipelineCreateInfo initInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .stage = initStage,
		        .layout = m_computeLayout,
		};
		if (vkCreateComputePipelines(device, m_context->GetPipelineCache(), 1, &initInfo, nullptr, &m_initPipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			Throw(AetherError::Vulkan(0, "LightingManager: failed to create initTiles compute pipeline."));
		}
		vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_initPipeline), VK_OBJECT_TYPE_PIPELINE, "LightCull.InitTiles");

		const VkPipelineShaderStageCreateInfo cullStage{
		        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
		        .module = shaderModule,
		        .pName = "binLights",
		};
		const VkComputePipelineCreateInfo cullInfo{
		        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		        .stage = cullStage,
		        .layout = m_computeLayout,
		};
		if (vkCreateComputePipelines(device, m_context->GetPipelineCache(), 1, &cullInfo, nullptr, &m_cullPipeline) != VK_SUCCESS)
		{
			vkDestroyShaderModule(device, shaderModule, nullptr);
			Throw(AetherError::Vulkan(0, "LightingManager: failed to create binLights compute pipeline."));
		}
		vkutil::SetObjectName(device, reinterpret_cast<std::uint64_t>(m_cullPipeline), VK_OBJECT_TYPE_PIPELINE, "LightCull.BinLights");

		vkDestroyShaderModule(device, shaderModule, nullptr);
	}

	void LightingManager::ApplyShadowIndices(const std::uint32_t frameSlot, const std::span<const glm::vec2> shadowIndices)
	{
		AE_PROFILE_ZONE();
		auto& frame = m_buffers[frameSlot];
		if (!frame.lights || shadowIndices.empty())
		{
			return;
		}

		const std::size_t lightCount = frame.lights.GetSize() / sizeof(GpuLight);
		const std::size_t applyCount = std::min(lightCount, shadowIndices.size());
		GpuLight* mapped = static_cast<GpuLight*>(frame.lights.GetAllocationInfo().pMappedData);
		for (std::size_t i = 0; i < applyCount; ++i)
		{
			mapped[i].shadowIndex.x = shadowIndices[i].x; // shadowIndex
			mapped[i].shadowIndex.y = shadowIndices[i].y; // shadowStrength
		}
		AE_EXPECT_OR_THROW_VOID(frame.lights.FlushMapped());
	}

	void LightingManager::DisableForView(FrameConstants& fc) const
	{
		fc.tiledLightGridInfo = glm::uvec4(0u);
		fc.tiledLightBufferOffsets = glm::uvec4(0u);
	}
} // namespace aether
