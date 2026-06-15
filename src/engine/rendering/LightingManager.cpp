#include "rendering/LightingManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/common.hpp>
#include <vector>

#include "gpu/DescriptorSetLayoutOps.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/GpuTypes.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/ResourceRegistry.hpp"
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

		const gpu::Device device = static_cast<gpu::Device>(m_context->GetDevice().device);
		for (auto& frame: m_buffers)
		{
			frame.lights.Reset();
			frame.tileHeaders.Reset();
			frame.tileIndices.Reset();
			frame.lightsCapacity = 0;
			frame.headersCapacity = 0;
			frame.indicesCapacity = 0;
		}

		if (m_initPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_initPipelineHandle);
			m_initPipelineHandle = {};
		}
		if (m_cullPipelineHandle.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_cullPipelineHandle);
			m_cullPipelineHandle = {};
		}
		if (m_computeLayout != nullptr)
		{
			gpu::Factory::DestroyPipelineLayout(static_cast<gpu::Device>(device), m_computeLayout);
			m_computeLayout = nullptr;
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
	        const Camera& camera,
	        const GpuExtent2D extent,
	        FrameConstants& fc,
	        const bool enableBinningForView,
	        const std::span<const Renderer::PointLight> pointLights,
	        const std::span<const Renderer::SpotLight> spotLights) const
	{
		AE_PROFILE_ZONE();
		if (!enableBinningForView || extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
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

	// TODO(audit/P1.1): EnsureBuffers is a buffer-pool helper that calls
	// vulkan/ UniqueBuffer / VMA directly, which is why the body is full
	// of VkBufferCreateInfo / VmaAllocationCreateInfo / VkDeviceSize. It
	// should move to vulkan/LightingManagerBuffers.cpp so the engine
	// version of LightingManager stays vulkan-free.
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
		if (m_computeLayout != VK_NULL_HANDLE && m_initPipelineHandle.IsValid() && m_cullPipelineHandle.IsValid())
		{
			return;
		}

		const gpu::Device device = static_cast<gpu::Device>(m_context->GetDevice().device);
		const gpu::PipelineCache pipelineCache = static_cast<gpu::PipelineCache>(m_context->GetPipelineCache());

		// Create shared layout once.
		if (m_computeLayout == nullptr)
		{
			const gpu::PushConstantRange pushRange{
			        .stageFlags = gpu::ShaderStage::Compute,
			        .offset = 0,
			        .size = static_cast<std::uint32_t>(sizeof(LightingComputePush)),
			};
			const std::array<gpu::DescriptorSetLayout, 1> setLayoutHandles{m_setLayout};
			m_computeLayout = gpu::Factory::CreatePipelineLayout(static_cast<gpu::Device>(device),
			        {
			                .setLayouts = setLayoutHandles,
			                .pushConstantRanges = std::span<const gpu::PushConstantRange>(&pushRange, 1),
			        });
			if (m_computeLayout == nullptr)
			{
				Throw(AetherError::Vulkan(0, "LightingManager: failed to create compute pipeline layout."));
			}
		}

		if (!m_initPipelineHandle.IsValid())
		{
			m_initPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(static_cast<gpu::Device>(device),
			        static_cast<gpu::PipelineCache>(pipelineCache),
			        gpu::ComputePipelineDesc{
			                .shaderVfsPath = "shaders://tiled_light_cull.spv",
			                .shaderEntry = "initTiles",
			                .pushConstantSize = static_cast<std::uint32_t>(sizeof(LightingComputePush)),
			                .debugName = "LightCull.InitTiles",
			                .existingLayout = m_computeLayout,
			        });
			if (!m_initPipelineHandle.IsValid())
			{
				Throw(AetherError::Vulkan(0, "LightingManager: failed to create initTiles compute pipeline."));
			}
		}

		if (!m_cullPipelineHandle.IsValid())
		{
			m_cullPipelineHandle = gpu::ResourceRegistry::CreateComputePipeline(static_cast<gpu::Device>(device),
			        static_cast<gpu::PipelineCache>(pipelineCache),
			        gpu::ComputePipelineDesc{
			                .shaderVfsPath = "shaders://tiled_light_cull.spv",
			                .shaderEntry = "binLights",
			                .pushConstantSize = static_cast<std::uint32_t>(sizeof(LightingComputePush)),
			                .debugName = "LightCull.BinLights",
			                .existingLayout = m_computeLayout,
			        });
			if (!m_cullPipelineHandle.IsValid())
			{
				Throw(AetherError::Vulkan(0, "LightingManager: failed to create binLights compute pipeline."));
			}
		}
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

	void LightingManager::RegisterPasses(RenderGraph& graph)
	{
		EnsureComputePipeline();

		// Register external buffer handles. Actual buffers are updated per-frame via UpdateBufferHandles.
		m_rgLights = graph.RegisterBuffer(nullptr);
		m_rgTileHeaders = graph.RegisterBuffer(nullptr);
		m_rgTileIndices = graph.RegisterBuffer(nullptr);

		const auto initResolved = gpu::ResourceRegistry::ResolvePipeline(m_initPipelineHandle);
		const auto cullResolved = gpu::ResourceRegistry::ResolvePipeline(m_cullPipelineHandle);

		graph.AddComputePass("$Lighting.InitTiles")
		        .ReadBuffer(m_rgLights)
		        .WriteBuffer(m_rgTileHeaders)
		        .WriteBuffer(m_rgTileIndices)
		        .ExecuteCompute(
		                [this, initPipeline = initResolved.pipeline, initLayout = initResolved.layout](PassContext& ctx)
		                {
			                if (!m_lightDataReady)
			                {
				                return;
			                }
			                const auto frameSlot = static_cast<std::uint32_t>(ctx.frameIndex % kMaxFramesInFlight);
			                auto& frame = m_buffers[frameSlot];

			                gpu::CommandList cmd = ctx.recorder.View();

			                // Host-write visibility barrier for the light data buffer.
			                cmd.PipelineMemoryBarrier(gpu::PipelineStage::Host, gpu::AccessFlags::HostWrite, gpu::PipelineStage::ComputeShader, gpu::AccessFlags::ShaderStorageRead | gpu::AccessFlags::ShaderStorageWrite);

			                cmd.BindComputePipeline(initPipeline, initLayout);

			                const gpu::GpuDescriptorBufferInfo lightInfo{.buffer = frame.lights.Get(), .offset = 0, .range = frame.lights.GetSize()};
			                const gpu::GpuDescriptorBufferInfo headerInfo{.buffer = frame.tileHeaders.Get(), .offset = 0, .range = frame.tileHeaders.GetSize()};
			                const gpu::GpuDescriptorBufferInfo indexInfo{.buffer = frame.tileIndices.Get(), .offset = 0, .range = frame.tileIndices.GetSize()};
			                const std::array<gpu::GpuWriteDescriptorSet, 3> writes{{
			                        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = gpu::DescriptorType::StorageBuffer, .bufferInfo = &lightInfo},
			                        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = gpu::DescriptorType::StorageBuffer, .bufferInfo = &headerInfo},
			                        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = gpu::DescriptorType::StorageBuffer, .bufferInfo = &indexInfo},
			                }};
			                cmd.PushDescriptorSet(m_computeLayout, 0, std::span<const gpu::GpuWriteDescriptorSet>(writes));
			                cmd.PushConstantsRaw(m_computeLayout, gpu::ShaderStage::Compute, 0, gpu::AsPushConstantBytes(m_lightPush));
			                cmd.Dispatch(m_lightTileGroups, 1, 1);
		                });

		graph.AddComputePass("$Lighting.BinLights")
		        .ReadBuffer(m_rgLights)
		        .ReadWriteBuffer(m_rgTileHeaders)
		        .ReadWriteBuffer(m_rgTileIndices)
		        .ExecuteCompute(
		                [this, cullPipeline = cullResolved.pipeline, cullLayout = cullResolved.layout](PassContext& ctx)
		                {
			                if (!m_lightDataReady || m_lightLightGroups == 0)
			                {
				                return;
			                }
			                const auto frameSlot = static_cast<std::uint32_t>(ctx.frameIndex % kMaxFramesInFlight);
			                auto& frame = m_buffers[frameSlot];

			                gpu::CommandList cmd = ctx.recorder.View();
			                cmd.BindComputePipeline(cullPipeline, cullLayout);

			                const gpu::GpuDescriptorBufferInfo lightInfo{.buffer = frame.lights.Get(), .offset = 0, .range = frame.lights.GetSize()};
			                const gpu::GpuDescriptorBufferInfo headerInfo{.buffer = frame.tileHeaders.Get(), .offset = 0, .range = frame.tileHeaders.GetSize()};
			                const gpu::GpuDescriptorBufferInfo indexInfo{.buffer = frame.tileIndices.Get(), .offset = 0, .range = frame.tileIndices.GetSize()};
			                const std::array<gpu::GpuWriteDescriptorSet, 3> writes{{
			                        {.dstBinding = 0, .descriptorCount = 1, .descriptorType = gpu::DescriptorType::StorageBuffer, .bufferInfo = &lightInfo},
			                        {.dstBinding = 1, .descriptorCount = 1, .descriptorType = gpu::DescriptorType::StorageBuffer, .bufferInfo = &headerInfo},
			                        {.dstBinding = 2, .descriptorCount = 1, .descriptorType = gpu::DescriptorType::StorageBuffer, .bufferInfo = &indexInfo},
			                }};
			                cmd.PushDescriptorSet(m_computeLayout, 0, std::span<const gpu::GpuWriteDescriptorSet>(writes));
			                cmd.PushConstantsRaw(m_computeLayout, gpu::ShaderStage::Compute, 0, gpu::AsPushConstantBytes(m_lightPush));
			                cmd.Dispatch(m_lightLightGroups, 1, 1);
		                });

		m_rgPassesRegistered = true;
	}

	bool LightingManager::PrepareForRenderGraph(
	        const std::uint32_t frameSlot, const Camera& camera, const GpuExtent2D extent, FrameConstants& fc, const std::span<const Renderer::PointLight> pointLights, const std::span<const Renderer::SpotLight> spotLights)
	{
		if (extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
			m_lightDataReady = false;
			return false;
		}

		std::vector<GpuLight> lights;
		BuildLightList(lights, pointLights, spotLights);

		if (lights.empty() && pointLights.empty() && spotLights.empty())
		{
			DisableForView(fc);
			m_lightDataReady = false;
			return false;
		}

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
		m_lightPush.viewProj = proj * camera.GetViewMatrix();
		m_lightPush.params0 = glm::vec4(camera.GetNearPlane(), 0.5f * static_cast<float>(extent.height) * std::abs(proj[1][1]), static_cast<float>(extent.width), static_cast<float>(extent.height));
		m_lightPush.params1 = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		m_lightPush.params2 = glm::uvec4(m_maxLightsPerTile, 0u, 0u, 0u);

		m_lightTileGroups = static_cast<std::uint32_t>((tileCount + 63u) / 64u);
		m_lightLightGroups = static_cast<std::uint32_t>((lights.size() + 63u) / 64u);
		m_lightDataReady = true;

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, m_maxLightsPerTile);

		return true;
	}

	void LightingManager::UpdateBufferHandles(RenderGraph& graph, const std::uint32_t frameSlot) const
	{
		auto& frame = m_buffers[frameSlot];
		graph.UpdateExternalBuffer(m_rgLights, static_cast<void*>(frame.lights.Get()));
		graph.UpdateExternalBuffer(m_rgTileHeaders, static_cast<void*>(frame.tileHeaders.Get()));
		graph.UpdateExternalBuffer(m_rgTileIndices, static_cast<void*>(frame.tileIndices.Get()));
	}
} // namespace aether
