#include "vulkan/RenderGraphStorage.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <numeric>
#include <queue>

#include "gpu/BindlessManager.hpp"
#include "gpu/FrameTarget.hpp"
#include "gpu/GpuEnums.hpp"
#include "gpu/Semaphore.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "vulkan/QueueSubmit.hpp"

namespace aether
{
	static constexpr std::size_t kMaxInlineBarriers = 16;

	void RenderGraphStorage::Initialize(gpu::Device device, gpu::Allocator allocator)
	{
		Initialize(static_cast<VkDevice>(device), static_cast<VmaAllocator>(allocator));
	}

	void RenderGraphStorage::EnableAsyncCompute(gpu::Queue computeQueue, std::uint32_t computeQueueFamily)
	{
		EnableAsyncCompute(static_cast<VkQueue>(computeQueue), computeQueueFamily);
	}

	uint32_t RenderGraphStorage::RegisterExternalImage(gpu::Image image, gpu::ImageView view, gpu::ImageAspect aspect)
	{
		return RegisterExternalImage(static_cast<VkImage>(image), static_cast<VkImageView>(view), gpu::ToVk(aspect));
	}

	uint32_t RenderGraphStorage::RegisterExternalBuffer(gpu::Buffer buffer)
	{
		return RegisterExternalBuffer(static_cast<VkBuffer>(buffer));
	}

	void RenderGraphStorage::UpdateExternalBuffer(uint32_t idx, gpu::Buffer buffer)
	{
		UpdateExternalBuffer(idx, static_cast<VkBuffer>(buffer));
	}

	std::uint32_t RenderGraphStorage::EnsureBindlessSampled(uint32_t transientIdx, gpu::ImageLayout descriptorLayout)
	{
		return EnsureBindlessSampled(transientIdx, gpu::ToVk(descriptorLayout));
	}

	void RenderGraphStorage::Initialize(VkDevice device, VmaAllocator allocator)
	{
		AE_PROFILE_ZONE();
		m_device = device;
		m_allocator = allocator;
	}

	void RenderGraphStorage::Shutdown()
	{
		AE_PROFILE_ZONE();
		ShutdownComputeResources();

		for (auto& entry: m_transientImages)
		{
			if (entry.image.IsValid())
			{
				gpu::ResourceRegistry::Destroy(entry.image);
			}
			entry.aliasedEntryIndex = 0xFFFFFFFFu;
			entry.allocatedExtent = {};
		}
		m_transientImages.clear();
		m_externalImages.clear();

		for (auto& [key, cachedList]: m_imageCache)
		{
			for (auto& ci: cachedList)
			{
				if (ci.handle.IsValid())
				{
					gpu::ResourceRegistry::Destroy(ci.handle);
				}
			}
		}
		m_imageCache.clear();
		m_freeTransientSlots.clear();
		m_freeExternalSlots.clear();

		if (m_virtualBlock != VK_NULL_HANDLE)
		{
			vmaDestroyVirtualBlock(m_virtualBlock);
			m_virtualBlock = VK_NULL_HANDLE;
		}
		if (m_transientHeapAllocation != VK_NULL_HANDLE)
		{
			vmaFreeMemory(m_allocator, m_transientHeapAllocation);
			m_transientHeapAllocation = VK_NULL_HANDLE;
		}
		m_transientHeapCapacity = 0;

		for (VkEvent event: m_events)
		{
			if (event != VK_NULL_HANDLE)
			{
				vkDestroyEvent(m_device, event, nullptr);
			}
		}
		m_events.clear();
		m_freeEventSlots.clear();

		m_device = VK_NULL_HANDLE;
		m_allocator = VK_NULL_HANDLE;
	}

	void RenderGraphStorage::EnableAsyncCompute(VkQueue computeQueue, std::uint32_t computeQueueFamily)
	{
		AE_ASSERT_ALWAYS(m_device != VK_NULL_HANDLE, "RenderGraphStorage: Initialize() must be called before EnableAsyncCompute().");
		AE_ASSERT_ALWAYS(computeQueue != VK_NULL_HANDLE, "RenderGraphStorage: computeQueue must be valid.");

		m_computeQueue = computeQueue;
		m_computeQueueFamily = computeQueueFamily;

		m_crossQueueTimeline = gpu::CreateTimelineSemaphore({
		        .device = static_cast<gpu::Device>(m_device),
		        .initialValue = 0,
		        .debugName = "RenderGraph.CrossQueueTimeline",
		});
		if (m_crossQueueTimeline == nullptr)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to create cross-queue timeline semaphore."));
		}
		vkutil::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(m_crossQueueTimeline->semaphore), VK_OBJECT_TYPE_SEMAPHORE, "RenderGraph.CrossQueueTimeline");

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (std::size_t frameI = 0; frameI < m_computeFrames.size(); ++frameI)
		{
			auto& frame = m_computeFrames[frameI];

			const VkCommandPoolCreateInfo poolInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
			        .queueFamilyIndex = m_computeQueueFamily,
			};
			if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &frame.commandPool) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to create async compute command pool."));
			}

			const VkCommandBufferAllocateInfo allocInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			        .commandPool = frame.commandPool,
			        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			        .commandBufferCount = 1,
			};
			if (vkAllocateCommandBuffers(m_device, &allocInfo, &frame.commandBuffer) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to allocate async compute command buffer."));
			}

			if (vkCreateFence(m_device, &fenceInfo, nullptr, &frame.fence) != VK_SUCCESS)
			{
				Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to create async compute fence."));
			}

			const std::string suffix = "[" + std::to_string(frameI) + "]";
			vkutil::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(frame.commandBuffer), VK_OBJECT_TYPE_COMMAND_BUFFER, ("RenderGraph.AsyncCompute.Cmd" + suffix).c_str());
			vkutil::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(frame.fence), VK_OBJECT_TYPE_FENCE, ("RenderGraph.AsyncCompute.Fence" + suffix).c_str());
		}

		m_asyncComputeEnabled = true;
	}

	void RenderGraphStorage::BeginComputeCommandBuffer(std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(m_asyncComputeEnabled, "RenderGraphStorage: async compute not enabled.");
		auto& frame = m_computeFrames[frameIndex % kMaxFramesInFlight];

		if (vkWaitForFences(m_device, 1, &frame.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		{
			if (m_vulkanContext != nullptr)
			{
				AE_EXPECT_OR_THROW_VOID(m_vulkanContext->WaitIdle());
			}
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to wait for compute fence."));
		}
		if (vkResetFences(m_device, 1, &frame.fence) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to reset compute fence."));
		}
		if (vkResetCommandPool(m_device, frame.commandPool, 0) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to reset compute command pool."));
		}

		const VkCommandBufferBeginInfo beginInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};
		if (vkBeginCommandBuffer(frame.commandBuffer, &beginInfo) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to begin compute command buffer."));
		}
	}

	gpu::CommandBuffer RenderGraphStorage::GetComputeCommandBuffer(std::uint32_t frameIndex) const
	{
		return m_computeFrames[frameIndex % kMaxFramesInFlight].commandBuffer;
	}

	void RenderGraphStorage::EndComputeCommandBuffer(std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		VkCommandBuffer cmd = m_computeFrames[frameIndex % kMaxFramesInFlight].commandBuffer;
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to end compute command buffer."));
		}
	}

	void RenderGraphStorage::SubmitComputeQueue(std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		AE_ASSERT(m_asyncComputeEnabled, "RenderGraphStorage: async compute not enabled.");
		auto& frame = m_computeFrames[frameIndex % kMaxFramesInFlight];
		const std::uint64_t signalValue = ++m_crossQueueTimelineValue;

		const VkCommandBufferSubmitInfo cmdInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = frame.commandBuffer,
		};

		const VkSemaphoreSubmitInfo signalInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = m_crossQueueTimeline->semaphore,
		        .value = signalValue,
		        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
		};

		const VkSubmitInfo2 submitInfo{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmdInfo,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &signalInfo,
		};

		VkResult computeSubmit = VK_SUCCESS;
		{
			const std::lock_guard<std::mutex> queueLock(aether::vulkan::QueueSubmitMutex());
			computeSubmit = vkQueueSubmit2(m_computeQueue, 1, &submitInfo, frame.fence);
		}
		if (computeSubmit != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to submit compute queue."));
		}
	}

	void RenderGraphStorage::ShutdownComputeResources()
	{
		if (!m_asyncComputeEnabled)
		{
			return;
		}

		for (auto& frame: m_computeFrames)
		{
			if (frame.fence != VK_NULL_HANDLE)
			{
				vkDestroyFence(m_device, frame.fence, nullptr);
				frame.fence = VK_NULL_HANDLE;
			}
			if (frame.commandPool != VK_NULL_HANDLE)
			{
				vkDestroyCommandPool(m_device, frame.commandPool, nullptr);
				frame.commandPool = VK_NULL_HANDLE;
				frame.commandBuffer = VK_NULL_HANDLE;
			}
		}

		if (m_crossQueueTimeline != nullptr)
		{
			gpu::DestroyTimelineSemaphore(static_cast<gpu::Device>(m_device), m_crossQueueTimeline);
			m_crossQueueTimeline = nullptr;
		}

		m_crossQueueTimelineValue = 0;
		m_asyncComputeEnabled = false;
	}

	void RenderGraphStorage::BeginFrame(std::uint32_t frameIndex)
	{
		AE_PROFILE_ZONE();
		m_currentFrame = frameIndex % kMaxFramesInFlight;
		// Only the per-frame class is cleared here. Levels are refreshed by
		// RefreshTransientStats() and the cumulative counters must survive the frame.
		m_lastFrameStats.passCount = 0;
		m_lastFrameStats.barrierCount = 0;
	}

	uint32_t RenderGraphStorage::RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect)
	{
		uint32_t idx = 0;
		if (!m_freeExternalSlots.empty())
		{
			idx = m_freeExternalSlots.back();
			m_freeExternalSlots.pop_back();
			m_externalImages[idx] = {.image = image, .view = view, .aspect = aspect};
		}
		else
		{
			idx = static_cast<uint32_t>(m_externalImages.size());
			m_externalImages.push_back({.image = image, .view = view, .aspect = aspect});
		}
#ifndef NDEBUG
		SetTrackedLayout(static_cast<gpu::Image>(image), gpu::ImageLayout::Undefined);
#endif
		return idx;
	}

	VkImage RenderGraphStorage::GetExternalImageVk(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].image : VK_NULL_HANDLE;
	}

	gpu::Image RenderGraphStorage::GetExternalImage(uint32_t idx) const
	{
		return GetExternalImageVk(idx);
	}

	VkImageView RenderGraphStorage::GetExternalViewVk(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].view : VK_NULL_HANDLE;
	}

	gpu::ImageView RenderGraphStorage::GetExternalView(uint32_t idx) const
	{
		return GetExternalViewVk(idx);
	}

	VkImageAspectFlags RenderGraphStorage::GetExternalAspectVk(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].aspect : VK_IMAGE_ASPECT_COLOR_BIT;
	}

	gpu::ImageAspect RenderGraphStorage::GetExternalAspect(uint32_t idx) const
	{
		return gpu::FromVk(GetExternalAspectVk(idx));
	}

	void RenderGraphStorage::ReleaseExternal(uint32_t idx)
	{
		if (idx < m_externalImages.size())
		{
#ifndef NDEBUG
			if (m_externalImages[idx].image != VK_NULL_HANDLE)
			{
				EraseTrackedLayout(static_cast<gpu::Image>(m_externalImages[idx].image));
			}
#endif
			m_externalImages[idx] = {};
			m_freeExternalSlots.push_back(idx);
		}
	}

	void RenderGraphStorage::ClearExternalImages()
	{
#ifndef NDEBUG
		for (const auto& entry: m_externalImages)
		{
			if (entry.image != VK_NULL_HANDLE)
			{
				EraseTrackedLayout(static_cast<gpu::Image>(entry.image));
			}
		}
#endif
		m_externalImages.clear();
		m_freeExternalSlots.clear();
	}

	uint32_t RenderGraphStorage::RegisterExternalBuffer(VkBuffer buffer)
	{
		if (!m_freeExternalBufferSlots.empty())
		{
			const uint32_t idx = m_freeExternalBufferSlots.back();
			m_freeExternalBufferSlots.pop_back();
			m_externalBuffers[idx] = buffer;
			return idx;
		}
		const auto idx = static_cast<uint32_t>(m_externalBuffers.size());
		m_externalBuffers.push_back(buffer);
		return idx;
	}

	void RenderGraphStorage::UpdateExternalBuffer(uint32_t idx, VkBuffer buffer)
	{
		if (idx >= m_externalBuffers.size())
		{
			m_externalBuffers.resize(idx + 1, VK_NULL_HANDLE);
		}
		m_externalBuffers[idx] = buffer;
	}

	VkBuffer RenderGraphStorage::GetExternalBufferVk(uint32_t idx) const
	{
		if (idx >= m_externalBuffers.size())
		{
			return VK_NULL_HANDLE;
		}
		return m_externalBuffers[idx];
	}

	gpu::Buffer RenderGraphStorage::GetExternalBuffer(uint32_t idx) const
	{
		return GetExternalBufferVk(idx);
	}

	void RenderGraphStorage::ReleaseExternalBuffer(uint32_t idx)
	{
		if (idx < m_externalBuffers.size())
		{
			m_externalBuffers[idx] = VK_NULL_HANDLE;
			m_freeExternalBufferSlots.push_back(idx);
		}
	}

	void RenderGraphStorage::ClearExternalBuffers()
	{
		m_externalBuffers.clear();
		m_freeExternalBufferSlots.clear();
	}

	uint32_t RenderGraphStorage::AddTransientSlot(gpu::Format format, gpu::ImageUsage usage, gpu::ImageAspect aspect, gpu::Extent2D extent)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: AddTransientSlot called before Initialize().");
		}

		if (!m_freeTransientSlots.empty())
		{
			const uint32_t idx = m_freeTransientSlots.back();
			m_freeTransientSlots.pop_back();
			auto& entry = m_transientImages[idx];
			entry = {};
			entry.format = format;
			entry.usage = usage;
			entry.aspect = aspect;
			entry.extent = extent;
			return idx;
		}

		TransientImageEntry entry{};
		entry.format = format;
		entry.usage = usage;
		entry.aspect = aspect;
		entry.extent = extent;
		m_transientImages.push_back(entry);
		return static_cast<uint32_t>(m_transientImages.size() - 1);
	}

	VkImage RenderGraphStorage::ResolveTransientImageVk(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return VK_NULL_HANDLE;
		}
		const auto& entry = m_transientImages[idx];
		if (entry.image.IsValid())
		{
			return static_cast<VkImage>(gpu::ResourceRegistry::ResolveTextureImage(entry.image));
		}
		if (entry.aliasedEntryIndex < m_transientImages.size())
		{
			const auto& aliased = m_transientImages[entry.aliasedEntryIndex];
			AE_ASSERT(aliased.image.IsValid(), "Alias target must own image - check aliasing logic");
			return static_cast<VkImage>(gpu::ResourceRegistry::ResolveTextureImage(aliased.image));
		}
		return VK_NULL_HANDLE;
	}

	VkImageView RenderGraphStorage::ResolveTransientViewVk(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return VK_NULL_HANDLE;
		}
		const auto& entry = m_transientImages[idx];
		if (entry.image.IsValid())
		{
			return static_cast<VkImageView>(gpu::ResourceRegistry::ResolveTexture(entry.image).view);
		}
		if (entry.aliasedEntryIndex < m_transientImages.size())
		{
			const auto& aliased = m_transientImages[entry.aliasedEntryIndex];
			AE_ASSERT(aliased.image.IsValid(), "Alias target must own image - check aliasing logic");
			return static_cast<VkImageView>(gpu::ResourceRegistry::ResolveTexture(aliased.image).view);
		}
		return VK_NULL_HANDLE;
	}

	VkImageAspectFlags RenderGraphStorage::ResolveTransientAspectVk(uint32_t idx) const
	{
		if (idx < m_transientImages.size())
		{
			return gpu::ToVk(m_transientImages[idx].aspect);
		}
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}

	gpu::ImageAspect RenderGraphStorage::ResolveTransientAspect(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return gpu::ImageAspect::Color;
		}
		return m_transientImages[idx].aspect;
	}

	gpu::Image RenderGraphStorage::ResolveTransientImage(uint32_t idx) const
	{
		return ResolveTransientImageVk(idx);
	}

	gpu::ImageView RenderGraphStorage::ResolveTransientView(uint32_t idx) const
	{
		return ResolveTransientViewVk(idx);
	}

	gpu::Extent2D RenderGraphStorage::GetTransientAllocatedExtent(uint32_t idx) const
	{
		if (idx < m_transientImages.size())
		{
			return m_transientImages[idx].allocatedExtent;
		}
		return {};
	}

	bool RenderGraphStorage::IsTransientSlotValid(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return false;
		}
		const auto& entry = m_transientImages[idx];
		return entry.image.IsValid() || entry.aliasedEntryIndex < m_transientImages.size();
	}

	std::uint32_t RenderGraphStorage::EnsureBindlessSampled(uint32_t transientIdx, VkImageLayout descriptorLayout)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			return 0xFFFFFFFFu;
		}
		if (transientIdx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		auto& entry = m_transientImages[transientIdx];
		entry.bindlessRequested = true;
		entry.bindlessLayout = descriptorLayout;
		entry.aliasedEntryIndex = 0xFFFFFFFFu;

		if (!entry.image.IsValid())
		{
			if (entry.format == gpu::Format::Undefined || static_cast<std::uint32_t>(entry.usage) == 0 || entry.extent.width == 0 || entry.extent.height == 0)
			{
				return 0xFFFFFFFFu;
			}

			const std::string bsName = std::format("RenderGraph.Transient.Bindless[{}]", transientIdx);
			entry.image = gpu::ResourceRegistry::CreateTexture(gpu::TextureDesc{
			        .format = entry.format,
			        .extent = entry.extent,
			        .usage = entry.usage,
			        .aspect = entry.aspect,
			        .mipLevels = 1,
			        .arrayLayers = 1,
			        .debugName = bsName.c_str(),
			});
			if (!entry.image.IsValid())
			{
				return 0xFFFFFFFFu;
			}
			entry.allocatedExtent = entry.extent;
			// A slot requested at graph-construction time cannot come out of the transient
			// heap: the heap plan does not exist yet. It is still a transient allocation and
			// has to be counted as one, or the totals lie about where the memory went.
			m_lastFrameStats.transientAllocated++;
			m_lastFrameStats.transientCacheMiss++;
		}

		gpu::ResourceRegistry::EnsureBindlessSampled(entry.image, entry.aspect, gpu::FromVk(descriptorLayout));
		return gpu::ResourceRegistry::GetBindlessSampledSlot(entry.image);
	}

	std::uint32_t RenderGraphStorage::GetBindlessSampledSlot(uint32_t transientIdx) const
	{
		if (transientIdx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		const auto& entry = m_transientImages[transientIdx];
		if (!gpu::ResourceRegistry::HasBindlessSampled(entry.image))
		{
			return 0xFFFFFFFFu;
		}
		return gpu::ResourceRegistry::GetBindlessSampledSlot(entry.image);
	}

	void RenderGraphStorage::ReleaseTransient(uint32_t idx)
	{
		if (idx >= m_transientImages.size())
		{
			return;
		}

		auto& entry = m_transientImages[idx];

		if (entry.fromHeap)
		{
			if (entry.image.IsValid())
			{
				gpu::ResourceRegistry::Destroy(entry.image);
			}
			if (entry.m_virtualAlloc)
			{
				vmaVirtualFree(m_virtualBlock, entry.m_virtualAlloc);
				entry.m_virtualAlloc = nullptr;
			}
		}
		else if (entry.bindlessRequested)
		{
#ifndef NDEBUG
			if (entry.image.IsValid())
			{
				EraseTrackedLayout(gpu::ResourceRegistry::ResolveTextureImage(entry.image));
			}
#endif
			if (entry.image.IsValid())
			{
				gpu::ResourceRegistry::Destroy(entry.image);
			}
		}
		else
		{
			MoveToCache(entry);
		}
		entry.bindlessRequested = false;
		entry.bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		entry.aliasedEntryIndex = 0xFFFFFFFFu;
		entry.fromHeap = false;
		entry.format = gpu::Format::Undefined;
		entry.usage = gpu::ImageUsage::None;
		entry.aspect = gpu::ImageAspect::Color;
		entry.extent = {};
		m_freeTransientSlots.push_back(idx);
	}

	RenderGraphStorage::ImageCacheKey RenderGraphStorage::MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent)
	{
		return ImageCacheKey{
		        .format = entry.format,
		        .usage = entry.usage,
		        .aspect = entry.aspect,
		        .width = extent.width,
		        .height = extent.height,
		        .mipLevels = 1,
		        .samples = VK_SAMPLE_COUNT_1_BIT,
		};
	}

	void RenderGraphStorage::MoveToCache(TransientImageEntry& entry)
	{
		if (!entry.image.IsValid() || entry.fromHeap)
		{
			return;
		}
#ifndef NDEBUG
		EraseTrackedLayout(gpu::ResourceRegistry::ResolveTextureImage(entry.image));
#endif
		const ImageCacheKey key = MakeCacheKey(entry, entry.allocatedExtent);
		m_imageCache[key].push_back(CachedImage{
		        .handle = entry.image,
		        .lastUsedFrame = m_currentFrame,
		});
		entry.image = {};
		entry.allocatedExtent = {};
	}

	gpu::TextureHandle RenderGraphStorage::TryPullFromCache(const ImageCacheKey& key)
	{
		auto it = m_imageCache.find(key);
		if (it == m_imageCache.end() || it->second.empty())
		{
			return {};
		}
		gpu::TextureHandle h = it->second.back().handle;
		it->second.pop_back();
		if (it->second.empty())
		{
			m_imageCache.erase(it);
		}
		return h;
	}

	void RenderGraphStorage::EvictStaleCacheEntries()
	{
		for (auto it = m_imageCache.begin(); it != m_imageCache.end();)
		{
			auto& list = it->second;
			for (auto ciIt = list.begin(); ciIt != list.end();)
			{
				const std::uint32_t age = (m_currentFrame >= ciIt->lastUsedFrame) ? (m_currentFrame - ciIt->lastUsedFrame) : (kMaxFramesInFlight + m_currentFrame - ciIt->lastUsedFrame);
				if (age > kCacheMaxStaleFrames)
				{
					if (ciIt->handle.IsValid())
					{
						gpu::ResourceRegistry::Destroy(ciIt->handle);
					}
					ciIt = list.erase(ciIt);
				}
				else
				{
					++ciIt;
				}
			}
			if (list.empty())
			{
				it = m_imageCache.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	std::uint32_t RenderGraphStorage::AllocateEvent()
	{
		if (m_device == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Vulkan, "RenderGraph: AllocateEvent called before Initialize.");
			return UINT32_MAX;
		}

		if (!m_freeEventSlots.empty())
		{
			const uint32_t idx = m_freeEventSlots.back();
			m_freeEventSlots.pop_back();
			return idx;
		}

		const auto idx = static_cast<uint32_t>(m_events.size());
		VkEvent event = VK_NULL_HANDLE;
		const VkEventCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
		        .flags = VK_EVENT_CREATE_DEVICE_ONLY_BIT,
		};
		if (vkCreateEvent(m_device, &info, nullptr, &event) != VK_SUCCESS)
		{
			AE_WARN(LogCategory::Vulkan, "RenderGraph: failed to create VkEvent for split barrier.");
			return UINT32_MAX;
		}
		m_events.push_back(event);
		const std::string eventName = std::format("RG.SplitEvent[{}]", idx);
		vkutil::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(event), VK_OBJECT_TYPE_EVENT, eventName.c_str());
		return idx;
	}

	VkEvent RenderGraphStorage::GetEventVk(std::uint32_t eventIndex) const
	{
		if (eventIndex < m_events.size())
		{
			return m_events[eventIndex];
		}
		return VK_NULL_HANDLE;
	}

	gpu::Event RenderGraphStorage::GetEvent(std::uint32_t eventIndex) const
	{
		return GetEventVk(eventIndex);
	}

	void RenderGraphStorage::ReleaseEvent(std::uint32_t eventIndex)
	{
		if (eventIndex < m_events.size())
		{
			m_freeEventSlots.push_back(eventIndex);
		}
	}

	void RenderGraphStorage::ResetEvents()
	{
		m_freeEventSlots.clear();
		m_freeEventSlots.reserve(m_events.size());
		for (std::uint32_t i = 0; i < m_events.size(); ++i)
		{
			m_freeEventSlots.push_back(i);
		}
	}

	void RenderGraphStorage::CmdSetEvent2(gpu::CommandBuffer cmd, gpu::Event event, std::span<const gpu::ImageMemoryBarrier> barriers)
	{
		if (barriers.empty())
		{
			return;
		}
		auto* vkCmd = static_cast<VkCommandBuffer>(cmd);
		auto* vkEvent = static_cast<VkEvent>(event);

		// signaled' validation warning and ensure the dependency info in
		vkCmdResetEvent2(vkCmd, vkEvent, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

		{
			const VkMemoryBarrier2 execBarrier{
			        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
			        .pNext = nullptr,
			        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			        .srcAccessMask = VK_ACCESS_2_NONE,
			        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
			        .dstAccessMask = VK_ACCESS_2_NONE,
			};
			const VkDependencyInfo execDep{
			        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			        .pNext = nullptr,
			        .memoryBarrierCount = 1,
			        .pMemoryBarriers = &execBarrier,
			        .bufferMemoryBarrierCount = 0,
			        .pBufferMemoryBarriers = nullptr,
			        .imageMemoryBarrierCount = 0,
			        .pImageMemoryBarriers = nullptr,
			};
			vkCmdPipelineBarrier2(vkCmd, &execDep);
		}

		VkImageMemoryBarrier2 vkBarriersInline[kMaxInlineBarriers];
		std::vector<VkImageMemoryBarrier2> vkBarriersHeap;
		auto* vkBarriers = vkBarriersInline;
		if (barriers.size() > kMaxInlineBarriers)
		{
			vkBarriersHeap.resize(barriers.size());
			vkBarriers = vkBarriersHeap.data();
		}
		for (std::size_t i = 0; i < barriers.size(); ++i)
		{
			vkBarriers[i] = gpu::ToVk(barriers[i], static_cast<VkImage>(barriers[i].image));
		}

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .pNext = nullptr,
		        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
		        .pImageMemoryBarriers = vkBarriers,
		};
		vkCmdSetEvent2(vkCmd, vkEvent, &depInfo);
	}

	void RenderGraphStorage::CmdWaitEvents2(gpu::CommandBuffer cmd, gpu::Event event, std::span<const gpu::ImageMemoryBarrier> barriers)
	{
		if (barriers.empty())
		{
			return;
		}
		auto* vkCmd = static_cast<VkCommandBuffer>(cmd);
		auto* vkEvent = static_cast<VkEvent>(event);

		VkImageMemoryBarrier2 vkBarriersInline[kMaxInlineBarriers];
		std::vector<VkImageMemoryBarrier2> vkBarriersHeap;
		auto* vkBarriers = vkBarriersInline;
		if (barriers.size() > kMaxInlineBarriers)
		{
			vkBarriersHeap.resize(barriers.size());
			vkBarriers = vkBarriersHeap.data();
		}
		for (std::size_t i = 0; i < barriers.size(); ++i)
		{
			vkBarriers[i] = gpu::ToVk(barriers[i], static_cast<VkImage>(barriers[i].image));
		}

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
		        .pImageMemoryBarriers = vkBarriers,
		};
		vkCmdWaitEvents2(vkCmd, 1, &vkEvent, &depInfo);
	}

	void RenderGraphStorage::CmdBufferBarriers(gpu::CommandBuffer cmd, std::span<const gpu::BufferMemoryBarrier> barriers)
	{
		if (barriers.empty())
		{
			return;
		}
		auto* vkCmd = static_cast<VkCommandBuffer>(cmd);

		VkBufferMemoryBarrier2 vkBarriersInline[kMaxInlineBarriers];
		std::vector<VkBufferMemoryBarrier2> vkBarriersHeap;
		auto* vkBarriers = vkBarriersInline;
		if (barriers.size() > kMaxInlineBarriers)
		{
			vkBarriersHeap.resize(barriers.size());
			vkBarriers = vkBarriersHeap.data();
		}
		for (std::size_t i = 0; i < barriers.size(); ++i)
		{
			vkBarriers[i] = gpu::ToVk(barriers[i], static_cast<VkBuffer>(barriers[i].buffer));
		}

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
		        .pBufferMemoryBarriers = vkBarriers,
		};
		vkCmdPipelineBarrier2(vkCmd, &depInfo);
	}

	void RenderGraphStorage::CmdImageBarriers(gpu::CommandBuffer cmd, std::span<const gpu::ImageMemoryBarrier> barriers)
	{
		if (barriers.empty())
		{
			return;
		}
		auto* vkCmd = static_cast<VkCommandBuffer>(cmd);

		VkImageMemoryBarrier2 vkBarriersInline[kMaxInlineBarriers];
		std::vector<VkImageMemoryBarrier2> vkBarriersHeap;
		auto* vkBarriers = vkBarriersInline;
		if (barriers.size() > kMaxInlineBarriers)
		{
			vkBarriersHeap.resize(barriers.size());
			vkBarriers = vkBarriersHeap.data();
		}
		for (std::size_t i = 0; i < barriers.size(); ++i)
		{
			vkBarriers[i] = gpu::ToVk(barriers[i], static_cast<VkImage>(barriers[i].image));
		}

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
		        .pImageMemoryBarriers = vkBarriers,
		};
		vkCmdPipelineBarrier2(vkCmd, &depInfo);
	}

	void RenderGraphStorage::AllocateTransientHeap(VkDeviceSize requiredSize, VkDeviceSize alignment)
	{
		if (m_virtualBlock != VK_NULL_HANDLE)
		{
			vmaDestroyVirtualBlock(m_virtualBlock);
			m_virtualBlock = VK_NULL_HANDLE;
		}
		if (m_transientHeapAllocation != VK_NULL_HANDLE)
		{
			vmaFreeMemory(m_allocator, m_transientHeapAllocation);
			m_transientHeapAllocation = VK_NULL_HANDLE;
		}
		m_transientHeapCapacity = 0;
		m_transientHeapAlignment = kTransientHeapAlignment;

		if (requiredSize == 0)
		{
			return;
		}

		const VkMemoryRequirements memReqs{
		        .size = requiredSize,
		        .alignment = alignment,
		        .memoryTypeBits = std::numeric_limits<std::uint32_t>::max(),
		};
		const VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_AUTO,
		        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		};
		if (vmaAllocateMemory(m_allocator, &memReqs, &allocInfo, &m_transientHeapAllocation, nullptr) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "RenderGraph: failed to allocate {} byte transient heap for VRAM aliasing.", requiredSize);
			m_transientHeapAllocation = VK_NULL_HANDLE;
			return;
		}
		m_transientHeapCapacity = requiredSize;
		m_transientHeapAlignment = alignment;

		const VmaVirtualBlockCreateInfo blockInfo{
		        .size = requiredSize,
		};
		if (vmaCreateVirtualBlock(&blockInfo, &m_virtualBlock) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "RenderGraph: failed to create VmaVirtualBlock.");
			vmaFreeMemory(m_allocator, m_transientHeapAllocation);
			m_transientHeapAllocation = VK_NULL_HANDLE;
			m_transientHeapCapacity = 0;
			m_transientHeapAlignment = kTransientHeapAlignment;
		}
	}

	void RenderGraphStorage::PrepareTransientAllocations(const FrameTarget& target)
	{
		AE_PROFILE_ZONE();
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			return;
		}

		// Only entries that still have to be materialised are re-planned. Wiping the heap
		// bookkeeping of a live entry orphaned its virtual allocation, left the image
		// sitting on a range the block no longer knew about, and made every "is this
		// aliased?" answer come back false from the second frame onwards.
		for (auto& entry: m_transientImages)
		{
			if (entry.image.IsValid())
			{
				continue;
			}
			entry.fromHeap = false;
			entry.m_virtualAlloc = nullptr;
			entry.heapOffset = VK_WHOLE_SIZE;
			entry.memReqSize = 0;
			entry.memReqAlignment = 0;
		}
		for (auto& entry: m_transientBuffers)
		{
			if (entry.buffer.IsValid())
			{
				continue;
			}
			entry.fromHeap = false;
			entry.m_virtualAlloc = nullptr;
			entry.heapOffset = VK_WHOLE_SIZE;
			entry.memReqSize = 0;
			entry.memReqAlignment = 0;
		}

		// Memory requirements are queried for every slot, not only the ones about to be
		// created: they are what the byte-level stats are computed from, and a slot that
		// was materialised eagerly (bindless) would otherwise report as costing nothing.
		for (auto& entry: m_transientImages)
		{
			if (entry.memReqSize != 0)
			{
				continue;
			}
			if (entry.format == gpu::Format::Undefined || static_cast<std::uint32_t>(entry.usage) == 0)
			{
				continue;
			}
			if (entry.extent.width == 0 || entry.extent.height == 0)
			{
				entry.extent = target.extent;
			}
			if (entry.extent.width == 0 || entry.extent.height == 0)
			{
				continue;
			}

			const VkImageCreateInfo tempInfo{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			        .flags = VK_IMAGE_CREATE_ALIAS_BIT,
			        .imageType = VK_IMAGE_TYPE_2D,
			        .format = gpu::ToVk(entry.format),
			        .extent = {.width = entry.extent.width, .height = entry.extent.height, .depth = 1u},
			        .mipLevels = 1,
			        .arrayLayers = 1,
			        .samples = VK_SAMPLE_COUNT_1_BIT,
			        .tiling = VK_IMAGE_TILING_OPTIMAL,
			        .usage = gpu::ToVk(entry.usage),
			        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			const VkDeviceImageMemoryRequirements query{
			        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
			        .pCreateInfo = &tempInfo,
			};
			VkMemoryRequirements2 reqs2{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
			vkGetDeviceImageMemoryRequirements(m_device, &query, &reqs2);
			entry.memReqSize = reqs2.memoryRequirements.size;
			entry.memReqAlignment = reqs2.memoryRequirements.alignment;
		}

		for (auto& entry: m_transientBuffers)
		{
			if (entry.memReqSize != 0)
			{
				continue;
			}
			if (entry.size == 0)
			{
				continue;
			}

			const VkBufferUsageFlags2CreateInfo tempUsageFlags2{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
			        .usage = entry.usage,
			};

			const VkBufferCreateInfo tempInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			        .pNext = &tempUsageFlags2,
			        .size = entry.size,
			        .usage = 0,
			};
			const VkDeviceBufferMemoryRequirements query{
			        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
			        .pCreateInfo = &tempInfo,
			};
			VkMemoryRequirements2 reqs2{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
			vkGetDeviceBufferMemoryRequirements(m_device, &query, &reqs2);
			entry.memReqSize = reqs2.memoryRequirements.size;
			entry.memReqAlignment = reqs2.memoryRequirements.alignment;
		}

		// Calculate total size needed. Process entries in alignment-descending
		auto collectCandidates = [](std::vector<std::uint32_t>& indices, const auto& entries, auto&& isValid, auto&& isAllocatable)
		{
			indices.clear();
			indices.reserve(entries.size());
			for (std::uint32_t i = 0; i < entries.size(); ++i)
			{
				if (!isValid(entries[i]) && isAllocatable(entries[i]))
				{
					indices.push_back(i);
				}
			}
		};

		auto& imageIndices = m_scratchTransientImageIndices;
		auto& bufferIndices = m_scratchTransientBufferIndices;
		collectCandidates(imageIndices, m_transientImages, [](const TransientImageEntry& e) { return e.image.IsValid(); }, [](const TransientImageEntry& e) { return e.memReqSize > 0; });
		collectCandidates(bufferIndices, m_transientBuffers, [](const TransientBufferEntry& e) { return e.buffer.IsValid(); }, [](const TransientBufferEntry& e) { return e.memReqSize > 0; });

		auto byAlignmentDescImages = [&](std::uint32_t a, std::uint32_t b)
		{
			return m_transientImages[a].memReqAlignment > m_transientImages[b].memReqAlignment;
		};
		auto byAlignmentDescBuffers = [&](std::uint32_t a, std::uint32_t b)
		{
			return m_transientBuffers[a].memReqAlignment > m_transientBuffers[b].memReqAlignment;
		};
		std::ranges::sort(imageIndices, byAlignmentDescImages);
		std::ranges::sort(bufferIndices, byAlignmentDescBuffers);

		VkDeviceSize totalSize = 0;
		VkDeviceSize maxAlignment = kTransientHeapAlignment;
		auto accumulate = [](VkDeviceSize current, VkDeviceSize size, VkDeviceSize alignment) -> VkDeviceSize
		{
			if (size == 0)
			{
				return current;
			}
			const VkDeviceSize aligned = AlignUp(current, alignment);
			return aligned + size;
		};

		for (const auto idx: imageIndices)
		{
			const auto& entry = m_transientImages[idx];
			totalSize = accumulate(totalSize, entry.memReqSize, entry.memReqAlignment);
			maxAlignment = std::max(maxAlignment, entry.memReqAlignment);
		}
		for (const auto idx: bufferIndices)
		{
			const auto& entry = m_transientBuffers[idx];
			totalSize = accumulate(totalSize, entry.memReqSize, entry.memReqAlignment);
			maxAlignment = std::max(maxAlignment, entry.memReqAlignment);
		}

		// Re-sizing the heap frees the underlying device memory, so every offset handed out
		// of it dies with it. It may only happen while nothing is resident - otherwise the
		// live images would keep rendering into memory that no longer belongs to them.
		const bool hasResidents = std::ranges::any_of(m_transientImages, [](const TransientImageEntry& e) { return e.fromHeap && e.image.IsValid(); })
		        || std::ranges::any_of(m_transientBuffers, [](const TransientBufferEntry& e) { return e.fromHeap && e.buffer.IsValid(); });

		if (!hasResidents && (totalSize > m_transientHeapCapacity || maxAlignment > m_transientHeapAlignment))
		{
			AllocateTransientHeap(totalSize * 3 / 2, maxAlignment);
		}

		if (m_virtualBlock == VK_NULL_HANDLE)
		{
			return;
		}

		// Process in the same alignment-descending order as the size
		for (const auto idx: imageIndices)
		{
			auto& entry = m_transientImages[idx];
			const VmaVirtualAllocationCreateInfo allocInfo{
			        .size = entry.memReqSize,
			        .alignment = entry.memReqAlignment,
			};
			VkDeviceSize offset = VK_WHOLE_SIZE;
			if (vmaVirtualAllocate(m_virtualBlock, &allocInfo, &entry.m_virtualAlloc, &offset) == VK_SUCCESS)
			{
				entry.fromHeap = true;
				entry.heapOffset = offset;
			}
		}

		for (const auto idx: bufferIndices)
		{
			auto& entry = m_transientBuffers[idx];
			const VmaVirtualAllocationCreateInfo allocInfo{
			        .size = entry.memReqSize,
			        .alignment = entry.memReqAlignment,
			};
			VkDeviceSize offset = VK_WHOLE_SIZE;
			if (vmaVirtualAllocate(m_virtualBlock, &allocInfo, &entry.m_virtualAlloc, &offset) == VK_SUCCESS)
			{
				entry.fromHeap = true;
				entry.heapOffset = offset;
			}
		}

	}

	void RenderGraphStorage::RefreshTransientStats()
	{
		AE_PROFILE_ZONE();
		FrameStats& stats = m_lastFrameStats;

		stats.transientImageCount = 0;
		stats.transientBufferCount = 0;
		stats.aliasedImageCount = 0;
		stats.aliasedBufferCount = 0;
		stats.transientLogicalBytes = 0;
		stats.transientPhysicalBytes = 0;

		for (const auto& entry: m_transientImages)
		{
			if (!entry.image.IsValid())
			{
				continue;
			}
			stats.transientImageCount++;
			stats.transientLogicalBytes += entry.memReqSize;
			if (entry.fromHeap)
			{
				stats.aliasedImageCount++;
			}
			else
			{
				stats.transientPhysicalBytes += entry.memReqSize;
			}
		}
		for (const auto& entry: m_transientBuffers)
		{
			if (!entry.buffer.IsValid())
			{
				continue;
			}
			stats.transientBufferCount++;
			stats.transientLogicalBytes += entry.memReqSize;
			if (entry.fromHeap)
			{
				stats.aliasedBufferCount++;
			}
			else
			{
				stats.transientPhysicalBytes += entry.memReqSize;
			}
		}

		stats.heapCapacity = m_transientHeapCapacity;
		stats.heapUsed = 0;
		if (m_virtualBlock != VK_NULL_HANDLE)
		{
			VmaStatistics blockStats{};
			vmaGetVirtualBlockStatistics(m_virtualBlock, &blockStats);
			stats.heapUsed = blockStats.allocationBytes;
		}
		// Everything the heap backs costs the heap's capacity, however many resources
		// share it - that is the whole point of the pool.
		stats.transientPhysicalBytes += m_transientHeapCapacity;

		stats.cacheSize = 0;
		for (const auto& [key, entries]: m_imageCache)
		{
			(void) key;
			stats.cacheSize += entries.size();
		}

		stats.pendingDestructions = gpu::ResourceRegistry::GetPendingDestructionCount();

		AE_VERBOSE(LogCategory::Vulkan,
		        "Transient heap: {:.1f} MB capacity, {:.1f} MB used, {} heap images, {} heap buffers, {} cache images",
		        static_cast<double>(stats.heapCapacity) / (1024.0 * 1024.0),
		        static_cast<double>(stats.heapUsed) / (1024.0 * 1024.0),
		        stats.aliasedImageCount,
		        stats.aliasedBufferCount,
		        stats.cacheSize);
	}

	void RenderGraphStorage::EnsureTransientImages(const FrameTarget& target)
	{
		AE_PROFILE_ZONE();
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: transient images require Initialize(device, allocator).");
			return;
		}

		for (std::uint32_t idx = 0; idx < m_transientImages.size(); ++idx)
		{
			auto& entry = m_transientImages[idx];
			if (entry.image.IsValid())
			{
				continue;
			}
			if (entry.format == gpu::Format::Undefined || static_cast<std::uint32_t>(entry.usage) == 0)
			{
				continue;
			}
			if (entry.extent.width == 0 || entry.extent.height == 0)
			{
				entry.extent = target.extent;
			}
			if (entry.extent.width == 0 || entry.extent.height == 0)
			{
				continue;
			}

			const ImageCacheKey key = MakeCacheKey(entry, entry.extent);
			const gpu::TextureHandle cached = TryPullFromCache(key);
			if (cached.IsValid())
			{
				entry.image = cached;
				entry.allocatedExtent = entry.extent;
				m_lastFrameStats.transientCacheHit++;
#ifndef NDEBUG
				SetTrackedLayout(gpu::ResourceRegistry::ResolveTextureImage(entry.image), gpu::ImageLayout::Undefined);
#endif
				continue;
			}

			if (entry.fromHeap && m_transientHeapAllocation != VK_NULL_HANDLE && entry.heapOffset != VK_WHOLE_SIZE)
			{
				const std::string entryName = entry.bindlessRequested ? std::format("RenderGraph.Transient.Aliased.Bindless[{}]", idx) : std::format("RenderGraph.Transient.Aliased[{}]", idx);
				entry.image = gpu::ResourceRegistry::CreateAliasedTexture(
				        gpu::TextureDesc{
				                .format = entry.format,
				                .extent = entry.extent,
				                .usage = entry.usage,
				                .aspect = entry.aspect,
				                .mipLevels = 1,
				                .arrayLayers = 1,
				        },
				        static_cast<void*>(m_transientHeapAllocation),
				        static_cast<gpu::DeviceSize>(entry.heapOffset),
				        entryName.c_str());
				if (!entry.image.IsValid())
				{
					continue;
				}
				entry.allocatedExtent = entry.extent;
				m_lastFrameStats.transientAllocated++;
				m_lastFrameStats.transientCacheMiss++;
#ifndef NDEBUG
				SetTrackedLayout(gpu::ResourceRegistry::ResolveTextureImage(entry.image), gpu::ImageLayout::Undefined);
#endif
				continue;
			}

			const std::string entryName = entry.bindlessRequested ? std::format("RenderGraph.Transient.Bindless[{}]", idx) : std::format("RenderGraph.Transient[{}]", idx);
			entry.image = gpu::ResourceRegistry::CreateTexture(gpu::TextureDesc{
			        .format = entry.format,
			        .extent = entry.extent,
			        .usage = entry.usage,
			        .aspect = entry.aspect,
			        .mipLevels = 1,
			        .arrayLayers = 1,
			        .debugName = entryName.c_str(),
			});
			if (!entry.image.IsValid())
			{
				continue;
			}
			entry.allocatedExtent = entry.extent;
			m_lastFrameStats.transientAllocated++;
			m_lastFrameStats.transientCacheMiss++;
#ifndef NDEBUG
			SetTrackedLayout(gpu::ResourceRegistry::ResolveTextureImage(entry.image), gpu::ImageLayout::Undefined);
#endif
		}

		EvictStaleCacheEntries();
	}

	uint32_t RenderGraphStorage::AddTransientBufferSlot(VkDeviceSize size, VkBufferUsageFlags2 usage)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: AddTransientBufferSlot called before Initialize().");
		}

		if (!m_freeTransientBufferSlots.empty())
		{
			const uint32_t idx = m_freeTransientBufferSlots.back();
			m_freeTransientBufferSlots.pop_back();
			auto& entry = m_transientBuffers[idx];
			entry = {};
			entry.size = size;
			entry.usage = usage;
			return idx;
		}

		TransientBufferEntry entry{};
		entry.size = size;
		entry.usage = usage;
		m_transientBuffers.push_back(entry);
		return static_cast<uint32_t>(m_transientBuffers.size() - 1);
	}

	void RenderGraphStorage::EnsureTransientBuffers()
	{
		AE_PROFILE_ZONE();
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: transient buffers require Initialize(device, allocator).");
			return;
		}

		for (std::uint32_t idx = 0; idx < m_transientBuffers.size(); ++idx)
		{
			auto& entry = m_transientBuffers[idx];
			if (entry.buffer.IsValid())
			{
				continue;
			}
			if (entry.size == 0)
			{
				continue;
			}

			if (entry.fromHeap && m_transientHeapAllocation != VK_NULL_HANDLE && entry.heapOffset != VK_WHOLE_SIZE)
			{
				const std::string entryName = std::format("RenderGraph.Transient.Buffer.Aliased[{}]", idx);
				entry.buffer = gpu::ResourceRegistry::CreateAliasedBuffer(
				        static_cast<gpu::DeviceSize>(entry.size), static_cast<gpu::BufferUsage>(entry.usage), static_cast<void*>(m_transientHeapAllocation), static_cast<gpu::DeviceSize>(entry.heapOffset), entryName.c_str());
				if (!entry.buffer.IsValid())
				{
					continue;
				}
				m_lastFrameStats.transientAllocated++;
				continue;
			}

			const std::string entryName = std::format("RenderGraph.Transient.Buffer[{}]", idx);
			entry.buffer = gpu::ResourceRegistry::CreateBuffer(gpu::BufferDesc{
			        .size = static_cast<gpu::DeviceSize>(entry.size),
			        .usage = static_cast<gpu::BufferUsage>(entry.usage),
			        .debugName = entryName.c_str(),
			});
			if (!entry.buffer.IsValid())
			{
				continue;
			}
			m_lastFrameStats.transientAllocated++;
		}
	}

	VkBuffer RenderGraphStorage::ResolveTransientBufferVk(uint32_t idx) const
	{
		if (idx < m_transientBuffers.size())
		{
			return static_cast<VkBuffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(m_transientBuffers[idx].buffer));
		}
		return VK_NULL_HANDLE;
	}

	gpu::Buffer RenderGraphStorage::ResolveTransientBuffer(uint32_t idx) const
	{
		return ResolveTransientBufferVk(idx);
	}

	bool RenderGraphStorage::IsTransientBufferSlotValid(uint32_t idx) const
	{
		if (idx >= m_transientBuffers.size())
		{
			return false;
		}
		return m_transientBuffers[idx].buffer.IsValid();
	}

	void RenderGraphStorage::ReleaseTransientBuffer(uint32_t idx)
	{
		if (idx >= m_transientBuffers.size())
		{
			return;
		}

		auto& entry = m_transientBuffers[idx];

		if (entry.fromHeap && entry.m_virtualAlloc)
		{
			vmaVirtualFree(m_virtualBlock, entry.m_virtualAlloc);
			entry.m_virtualAlloc = nullptr;
		}
		if (entry.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(entry.buffer);
		}

		entry = {};
		m_freeTransientBufferSlots.push_back(idx);
	}
} // namespace aether
