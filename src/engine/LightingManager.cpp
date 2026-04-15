#include "LightingManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <glm/common.hpp>

namespace aether
{
	void LightingManager::Initialize(const VulkanContext& context, const Renderer& renderer)
	{
		m_context = &context;
		m_renderer = &renderer;

		const VkDevice device = m_context->GetDevice().device;

		const VkDescriptorSetLayoutBinding bindings[] = {
			{
				.binding = 0,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
			},
			{
				.binding = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
			},
			{
				.binding = 2,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.descriptorCount = 1,
				.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
			},
		};

		const VkDescriptorSetLayoutCreateInfo layoutInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.bindingCount = static_cast<std::uint32_t>(std::size(bindings)),
			.pBindings = bindings,
		};
		if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &m_setLayout) != VK_SUCCESS)
		{
			throw std::runtime_error("LightingManager: failed to create lighting descriptor set layout.");
		}

		const VkDescriptorPoolSize poolSize{
			.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.descriptorCount = 3u * Swapchain::kMaxFramesInFlight,
		};
		const VkDescriptorPoolCreateInfo poolInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.maxSets = Swapchain::kMaxFramesInFlight,
			.poolSizeCount = 1,
			.pPoolSizes = &poolSize,
		};
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
		{
			throw std::runtime_error("LightingManager: failed to create lighting descriptor pool.");
		}

		std::array<VkDescriptorSetLayout, Swapchain::kMaxFramesInFlight> layouts{};
		layouts.fill(m_setLayout);
		const VkDescriptorSetAllocateInfo allocInfo{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = m_descriptorPool,
			.descriptorSetCount = Swapchain::kMaxFramesInFlight,
			.pSetLayouts = layouts.data(),
		};
		if (vkAllocateDescriptorSets(device, &allocInfo, m_sets.data()) != VK_SUCCESS)
		{
			throw std::runtime_error("LightingManager: failed to allocate lighting descriptor sets.");
		}

		for (std::uint32_t i = 0; i < Swapchain::kMaxFramesInFlight; ++i)
		{
			EnsureBuffers(i, 1, 1, 1);
			UpdateDescriptorSet(i);
		}
	}

	void LightingManager::Shutdown()
	{
		if (!m_context)
		{
			return;
		}

		const VkDevice device = m_context->GetDevice().device;
		for (auto& frame : m_buffers)
		{
			frame.lights.Reset();
			frame.tileHeaders.Reset();
			frame.tileIndices.Reset();
			frame.lightsCapacity = 0;
			frame.headersCapacity = 0;
			frame.indicesCapacity = 0;
		}

		if (m_descriptorPool != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
			m_descriptorPool = VK_NULL_HANDLE;
		}
		if (m_setLayout != VK_NULL_HANDLE)
		{
			vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
			m_setLayout = VK_NULL_HANDLE;
		}

		m_sets.fill(VK_NULL_HANDLE);
		m_context = nullptr;
		m_renderer = nullptr;
	}

	VkDescriptorSetLayout LightingManager::GetSetLayout() const
	{
		return m_setLayout;
	}

	VkDescriptorSet LightingManager::GetSet(const std::uint32_t frameSlot) const
	{
		return m_sets[frameSlot];
	}

	void LightingManager::UpdateForView(
		const std::uint32_t frameSlot,
		const Camera& camera,
		const VkExtent2D extent,
		FrameConstants& fc,
		const bool enableBinningForView) const
	{
		if (!enableBinningForView || extent.width == 0 || extent.height == 0)
		{
			DisableForView(fc);
			return;
		}

		std::vector<GpuLight> lights;
		lights.reserve(m_renderer->GetPointLights().size() + m_renderer->GetSpotLights().size());

		for (const Renderer::PointLight& src : m_renderer->GetPointLights())
		{
			lights.push_back(GpuLight{
				.positionRadius = glm::vec4(src.position, src.radius),
				.colorIntensity = glm::vec4(src.color, src.intensity),
				.directionType = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
				.params = glm::vec4(0.0f),
				});
		}
		for (const Renderer::SpotLight& src : m_renderer->GetSpotLights())
		{
			lights.push_back(GpuLight{
				.positionRadius = glm::vec4(src.position, src.radius),
				.colorIntensity = glm::vec4(src.color, src.intensity),
				.directionType = glm::vec4(glm::normalize(src.direction), 1.0f),
				.params = glm::vec4(std::cos(src.innerAngleRad), std::cos(src.outerAngleRad), 0.0f, 0.0f),
				});
		}

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
				const float screenY = (1.0f - (ndc.y * 0.5f + 0.5f)) * static_cast<float>(extent.height);
				const float radiusPx = (light.positionRadius.w * pixelScaleY) / std::max(depth, nearClip);
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
		UpdateDescriptorSet(frameSlot);

		fc.tiledLightGridInfo = glm::uvec4(kTileSizePx, tilesX, tilesY, static_cast<std::uint32_t>(lights.size()));
		fc.tiledLightBufferOffsets = glm::uvec4(0u, 0u, 0u, 0u);
	}

	void LightingManager::EnsureBuffers(
		const std::uint32_t frameSlot,
		const std::size_t lightCount,
		const std::size_t tileCount,
		const std::size_t indexCount) const
	{
		auto& frame = m_buffers[frameSlot];
		const VkDevice device = m_context->GetDevice().device;
		const VmaAllocator allocator = m_context->GetAllocator();

		auto ensureBuffer = [&](UniqueBuffer& buffer,
			std::size_t& capacity,
			const std::size_t required,
			const VkDeviceSize stride)
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

				buffer.Reset();
				VkBufferCreateInfo info{
					.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
					.size = stride * capacity,
					.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
				};
				VmaAllocationCreateInfo allocInfo{};
				allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
				allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
					VMA_ALLOCATION_CREATE_MAPPED_BIT;
				buffer = UniqueBuffer::Create(allocator, device, info, allocInfo);
			};

		ensureBuffer(frame.lights, frame.lightsCapacity, lightCount, sizeof(GpuLight));
		ensureBuffer(frame.tileHeaders, frame.headersCapacity, tileCount, sizeof(TileHeader));
		ensureBuffer(frame.tileIndices, frame.indicesCapacity, indexCount, sizeof(std::uint32_t));
	}

	void LightingManager::UpdateDescriptorSet(const std::uint32_t frameSlot) const
	{
		if (m_sets[frameSlot] == VK_NULL_HANDLE)
		{
			return;
		}

		auto& frame = m_buffers[frameSlot];
		const VkDescriptorBufferInfo lightInfo{
			.buffer = frame.lights.Get(),
			.offset = 0,
			.range = frame.lights.GetSize(),
		};
		const VkDescriptorBufferInfo headerInfo{
			.buffer = frame.tileHeaders.Get(),
			.offset = 0,
			.range = frame.tileHeaders.GetSize(),
		};
		const VkDescriptorBufferInfo indexInfo{
			.buffer = frame.tileIndices.Get(),
			.offset = 0,
			.range = frame.tileIndices.GetSize(),
		};

		const VkWriteDescriptorSet writes[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_sets[frameSlot],
				.dstBinding = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &lightInfo,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_sets[frameSlot],
				.dstBinding = 1,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &headerInfo,
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = m_sets[frameSlot],
				.dstBinding = 2,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &indexInfo,
			},
		};
		vkUpdateDescriptorSets(m_context->GetDevice().device,
			static_cast<std::uint32_t>(std::size(writes)), writes, 0, nullptr);
	}

	void LightingManager::DisableForView(FrameConstants& fc) const
	{
		fc.tiledLightGridInfo = glm::uvec4(0u);
		fc.tiledLightBufferOffsets = glm::uvec4(0u);
	}
}
