#include "vulkan/RenderGraphStorage.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <numeric>
#include <queue>

#include "gpu/BindlessManager.hpp"
#include "gpu/FrameTarget.hpp"
#include "gpu/GpuEnums.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "utils/Logger.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether
{
	// Engine-side forwarders: cast opaque gpu:: types to Vk* and delegate
	// to the Vulkan-internal overload. Keeps the bridge in one place.

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
		return RegisterExternalImage(
		        static_cast<VkImage>(image),
		        static_cast<VkImageView>(view),
		        gpu::ToVk(aspect));
	}

	uint32_t RenderGraphStorage::RegisterExternalBuffer(gpu::Buffer buffer)
	{
		return RegisterExternalBuffer(static_cast<VkBuffer>(buffer));
	}

	void RenderGraphStorage::UpdateExternalBuffer(uint32_t idx, gpu::Buffer buffer)
	{
		UpdateExternalBuffer(idx, static_cast<VkBuffer>(buffer));
	}

	std::uint32_t RenderGraphStorage::EnsureBindlessSampled(uint32_t transientIdx, BindlessManager& bindlessManager, gpu::Device device, gpu::ImageLayout descriptorLayout)
	{
		return EnsureBindlessSampled(
		        transientIdx,
		        bindlessManager,
		        static_cast<VkDevice>(device),
		        gpu::ToVk(descriptorLayout));
	}

	void RenderGraphStorage::Initialize(VkDevice device, VmaAllocator allocator)
	{
		m_device = device;
		m_allocator = allocator;
	}

	void RenderGraphStorage::Shutdown()
	{
		ShutdownComputeResources();

		for (auto& entry: m_transientImages)
		{
			entry.image.Reset();
			entry.aliasedEntryIndex = 0xFFFFFFFFu;
			entry.allocatedExtent = {};
		}
		m_transientImages.clear();
		m_externalImages.clear();

		for (auto& [key, cachedList]: m_imageCache)
		{
			for (auto& ci: cachedList)
			{
				ci.image.Reset();
			}
		}
		m_imageCache.clear();
		m_freeTransientSlots.clear();
		m_freeExternalSlots.clear();

		for (std::size_t i = 0; i < kMaxFramesInFlight; ++i)
		{
			m_pendingDestructions[i].clear();
		}

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

		const VkSemaphoreTypeCreateInfo timelineTypeInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		        .initialValue = 0,
		};
		const VkSemaphoreCreateInfo semInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		        .pNext = &timelineTypeInfo,
		};
		if (vkCreateSemaphore(m_device, &semInfo, nullptr, &m_crossQueueTimeline) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to create cross-queue timeline semaphore."));
		}
		vkutil::SetObjectName(m_device, reinterpret_cast<std::uint64_t>(m_crossQueueTimeline), VK_OBJECT_TYPE_SEMAPHORE, "RenderGraph.CrossQueueTimeline");

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (std::size_t frameI = 0; frameI < m_computeFrames.size(); ++frameI)
		{
			auto& frame = m_computeFrames[frameI];

			const VkCommandPoolCreateInfo poolInfo{
			        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
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
		AE_ASSERT(m_asyncComputeEnabled, "RenderGraphStorage: async compute not enabled.");
		auto& frame = m_computeFrames[frameIndex % kMaxFramesInFlight];

		if (vkWaitForFences(m_device, 1, &frame.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
		{
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

	VkCommandBuffer RenderGraphStorage::GetComputeCommandBuffer(std::uint32_t frameIndex) const
	{
		return m_computeFrames[frameIndex % kMaxFramesInFlight].commandBuffer;
	}

	void RenderGraphStorage::EndComputeCommandBuffer(std::uint32_t frameIndex)
	{
		VkCommandBuffer cmd = m_computeFrames[frameIndex % kMaxFramesInFlight].commandBuffer;
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
		{
			Throw(AetherError::Vulkan(0, "RenderGraphStorage: failed to end compute command buffer."));
		}
	}

	void RenderGraphStorage::SubmitComputeQueue(std::uint32_t frameIndex)
	{
		AE_ASSERT(m_asyncComputeEnabled, "RenderGraphStorage: async compute not enabled.");
		auto& frame = m_computeFrames[frameIndex % kMaxFramesInFlight];
		const std::uint64_t signalValue = ++m_crossQueueTimelineValue;

		const VkCommandBufferSubmitInfo cmdInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		        .commandBuffer = frame.commandBuffer,
		};

		const VkSemaphoreSubmitInfo signalInfo{
		        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		        .semaphore = m_crossQueueTimeline,
		        .value = signalValue,
		        .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
		};

		const VkSubmitInfo2 submitInfo{
		        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		        .commandBufferInfoCount = 1,
		        .pCommandBufferInfos = &cmdInfo,
		        .signalSemaphoreInfoCount = 1,
		        .pSignalSemaphoreInfos = &signalInfo,
		};

		if (vkQueueSubmit2(m_computeQueue, 1, &submitInfo, frame.fence) != VK_SUCCESS)
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

		if (m_crossQueueTimeline != VK_NULL_HANDLE)
		{
			vkDestroySemaphore(m_device, m_crossQueueTimeline, nullptr);
			m_crossQueueTimeline = VK_NULL_HANDLE;
		}

		m_crossQueueTimelineValue = 0;
		m_asyncComputeEnabled = false;
	}

	void RenderGraphStorage::BeginFrame(std::uint32_t frameIndex)
	{
		m_currentFrame = frameIndex % kMaxFramesInFlight;

		m_lastFrameStats = FrameStats{};

		auto& toDestroy = m_pendingDestructions[m_currentFrame];
		m_lastFrameStats.pendingDestructions = static_cast<std::uint32_t>(toDestroy.size());
		for (auto& pending: toDestroy)
		{
			pending.image.Reset();
		}
		toDestroy.clear();
	}

	// -- External images ------------------------------------------------------

	uint32_t RenderGraphStorage::RegisterExternalImage(VkImage image, VkImageView view, VkImageAspectFlags aspect)
	{
		uint32_t idx;
		if (!m_freeExternalSlots.empty())
		{
			idx = m_freeExternalSlots.back();
			m_freeExternalSlots.pop_back();
			m_externalImages[idx] = {image, view, aspect};
		}
		else
		{
			idx = static_cast<uint32_t>(m_externalImages.size());
			m_externalImages.push_back({image, view, aspect});
		}
#ifndef NDEBUG
		SetTrackedLayout(image, VK_IMAGE_LAYOUT_UNDEFINED);
#endif
		return idx;
	}

	VkImage RenderGraphStorage::GetExternalImage(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].image : VK_NULL_HANDLE;
	}

	VkImageView RenderGraphStorage::GetExternalView(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].view : VK_NULL_HANDLE;
	}

	VkImageAspectFlags RenderGraphStorage::GetExternalAspect(uint32_t idx) const
	{
		return (idx < m_externalImages.size()) ? m_externalImages[idx].aspect : VK_IMAGE_ASPECT_COLOR_BIT;
	}

	void RenderGraphStorage::ReleaseExternal(uint32_t idx)
	{
		if (idx < m_externalImages.size())
		{
#ifndef NDEBUG
			if (m_externalImages[idx].image != VK_NULL_HANDLE)
			{
				EraseTrackedLayout(m_externalImages[idx].image);
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
				EraseTrackedLayout(entry.image);
			}
		}
#endif
		m_externalImages.clear();
		m_freeExternalSlots.clear();
	}

	// -- External buffers -----------------------------------------------------

	uint32_t RenderGraphStorage::RegisterExternalBuffer(VkBuffer buffer)
	{
		if (!m_freeExternalBufferSlots.empty())
		{
			const uint32_t idx = m_freeExternalBufferSlots.back();
			m_freeExternalBufferSlots.pop_back();
			m_externalBuffers[idx] = buffer;
			return idx;
		}
		const uint32_t idx = static_cast<uint32_t>(m_externalBuffers.size());
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

	VkBuffer RenderGraphStorage::GetExternalBuffer(uint32_t idx) const
	{
		if (idx >= m_externalBuffers.size())
		{
			return VK_NULL_HANDLE;
		}
		return m_externalBuffers[idx];
	}

	void RenderGraphStorage::ReleaseExternalBuffer(uint32_t idx)
	{
		if (idx < m_externalBuffers.size())
		{
			m_externalBuffers[idx] = VK_NULL_HANDLE;
			m_freeExternalBufferSlots.push_back(idx);
		}
	}

	// -- Transient images -----------------------------------------------------

	uint32_t RenderGraphStorage::AddTransientSlot(VkFormat format, gpu::ImageUsage usage, gpu::ImageAspect aspect, gpu::Extent2D extent)
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
		m_transientImages.push_back(std::move(entry));
		return static_cast<uint32_t>(m_transientImages.size() - 1);
	}

	VkImage RenderGraphStorage::ResolveTransientImage(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return VK_NULL_HANDLE;
		}
		const auto& entry = m_transientImages[idx];
		if (entry.image)
		{
			return entry.image.Get();
		}
		if (entry.aliasedEntryIndex < m_transientImages.size())
		{
			AE_ASSERT(m_transientImages[entry.aliasedEntryIndex].image, "Alias target must own image - check aliasing logic");
			return m_transientImages[entry.aliasedEntryIndex].image.Get();
		}
		return VK_NULL_HANDLE;
	}

	VkImageView RenderGraphStorage::ResolveTransientView(uint32_t idx) const
	{
		if (idx >= m_transientImages.size())
		{
			return VK_NULL_HANDLE;
		}
		const auto& entry = m_transientImages[idx];
		if (entry.image)
		{
			return entry.image.GetDefaultView();
		}
		if (entry.aliasedEntryIndex < m_transientImages.size())
		{
			AE_ASSERT(m_transientImages[entry.aliasedEntryIndex].image, "Alias target must own image - check aliasing logic");
			return m_transientImages[entry.aliasedEntryIndex].image.GetDefaultView();
		}
		return VK_NULL_HANDLE;
	}

	VkImageAspectFlags RenderGraphStorage::ResolveTransientAspect(uint32_t idx) const
	{
		if (idx < m_transientImages.size())
		{
			return gpu::ToVk(m_transientImages[idx].aspect);
		}
		return VK_IMAGE_ASPECT_COLOR_BIT;
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
		return entry.image || entry.aliasedEntryIndex < m_transientImages.size();
	}

	// -- Bindless -------------------------------------------------------------

	std::uint32_t RenderGraphStorage::EnsureBindlessSampled(uint32_t transientIdx, BindlessManager& bindlessManager, VkDevice device, VkImageLayout descriptorLayout)
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

		if (!entry.image)
		{
			if (entry.format == VK_FORMAT_UNDEFINED || static_cast<std::uint32_t>(entry.usage) == 0 || entry.extent.width == 0 || entry.extent.height == 0)
			{
				return 0xFFFFFFFFu;
			}

			AE_EXPECT_OR_THROW(newImage,
			        UniqueImage::Create(m_device,
			                m_allocator,
			                {
			                        .extent = entry.extent,
			                        .format = gpu::FromVk(entry.format),
			                        .usage = entry.usage,
			                }));
			entry.image = std::move(newImage);
			entry.allocatedExtent = entry.extent;
		}

		AE_EXPECT_OR_THROW_VOID(entry.image.EnsureBindlessSampled(bindlessManager, device, entry.aspect, gpu::FromVk(descriptorLayout)));
		return entry.image.GetBindlessSampledSlot();
	}

	std::uint32_t RenderGraphStorage::GetBindlessSampledSlot(uint32_t transientIdx) const
	{
		if (transientIdx >= m_transientImages.size())
		{
			return 0xFFFFFFFFu;
		}

		const auto& entry = m_transientImages[transientIdx];
		if (!entry.image.HasBindlessSampled())
		{
			return 0xFFFFFFFFu;
		}
		return entry.image.GetBindlessSampledSlot();
	}

	// -- Release / cache ------------------------------------------------------

	void RenderGraphStorage::ReleaseTransient(uint32_t idx, std::uint32_t currentFrame)
	{
		if (idx >= m_transientImages.size())
		{
			return;
		}

		auto& entry = m_transientImages[idx];

		if (entry.fromHeap)
		{
			// Heap-backed images are not cached; destroy the VkImage handle.
			// The persistent heap allocation is reused next frame.
			entry.image.Reset();
		}
		else if (entry.bindlessRequested)
		{
#ifndef NDEBUG
			if (entry.image)
			{
				EraseTrackedLayout(entry.image.Get());
			}
#endif
			PendingDestruction pending{};
			pending.entryIndex = idx;
			pending.image = std::move(entry.image);
			m_pendingDestructions[currentFrame % kMaxFramesInFlight].push_back(std::move(pending));
		}
		else
		{
			MoveToCache(entry);
		}
		entry.bindlessRequested = false;
		entry.bindlessLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		entry.aliasedEntryIndex = 0xFFFFFFFFu;
		entry.fromHeap = false;
		entry.format = VK_FORMAT_UNDEFINED;
		entry.usage = gpu::ImageUsage::None;
		entry.aspect = gpu::ImageAspect::Color;
		entry.extent = {};
		m_freeTransientSlots.push_back(idx);
	}

	// -- Cache helpers --------------------------------------------------------

	RenderGraphStorage::ImageCacheKey RenderGraphStorage::MakeCacheKey(const TransientImageEntry& entry, gpu::Extent2D extent) const
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
		if (!entry.image || entry.fromHeap)
		{
			return;
		}
#ifndef NDEBUG
		EraseTrackedLayout(entry.image.Get());
#endif
		const ImageCacheKey key = MakeCacheKey(entry, entry.allocatedExtent);
		m_imageCache[key].push_back(CachedImage{
		        .image = std::move(entry.image),
		        .lastUsedFrame = m_currentFrame,
		});
		entry.allocatedExtent = {};
	}

	UniqueImage RenderGraphStorage::TryPullFromCache(const ImageCacheKey& key)
	{
		auto it = m_imageCache.find(key);
		if (it == m_imageCache.end() || it->second.empty())
		{
			return {};
		}
		UniqueImage img = std::move(it->second.back().image);
		it->second.pop_back();
		if (it->second.empty())
		{
			m_imageCache.erase(it);
		}
		return img;
	}

	void RenderGraphStorage::EvictStaleCacheEntries()
	{
		for (auto it = m_imageCache.begin(); it != m_imageCache.end();)
		{
			auto& list = it->second;
			std::erase_if(list,
			        [&](const CachedImage& ci)
			        {
				        const std::uint32_t age = (m_currentFrame >= ci.lastUsedFrame) ? (m_currentFrame - ci.lastUsedFrame) : (kMaxFramesInFlight + m_currentFrame - ci.lastUsedFrame);
				        return age > kCacheMaxStaleFrames;
			        });
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

	// -- Split barrier events --------------------------------------------------

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

		const uint32_t idx = static_cast<uint32_t>(m_events.size());
		VkEvent event = VK_NULL_HANDLE;
		const VkEventCreateInfo info{
		        .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
		};
		if (vkCreateEvent(m_device, &info, nullptr, &event) != VK_SUCCESS)
		{
			AE_WARN(LogCategory::Vulkan, "RenderGraph: failed to create VkEvent for split barrier.");
			return UINT32_MAX;
		}
		m_events.push_back(event);
		return idx;
	}

	VkEvent RenderGraphStorage::GetEvent(std::uint32_t eventIndex) const
	{
		if (eventIndex < m_events.size())
		{
			return m_events[eventIndex];
		}
		return VK_NULL_HANDLE;
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

	void RenderGraphStorage::CmdSetEvent2(VkCommandBuffer cmd, VkEvent event, const VkImageMemoryBarrier2* barriers, uint32_t count)
	{
		if (count == 0)
		{
			return;
		}

		vkCmdResetEvent2(cmd, event, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = count,
		        .pImageMemoryBarriers = barriers,
		};
		vkCmdSetEvent2(cmd, event, &depInfo);
	}

	void RenderGraphStorage::CmdWaitEvents2(VkCommandBuffer cmd, VkEvent event, const VkImageMemoryBarrier2* barriers, uint32_t count)
	{
		if (count == 0)
		{
			return;
		}

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .imageMemoryBarrierCount = count,
		        .pImageMemoryBarriers = barriers,
		};
		vkCmdWaitEvents2(cmd, 1, &event, &depInfo);
	}

	void RenderGraphStorage::CmdBufferBarriers(VkCommandBuffer cmd, const VkBufferMemoryBarrier2* barriers, uint32_t count)
	{
		if (count == 0)
		{
			return;
		}

		const VkDependencyInfo depInfo{
		        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
		        .bufferMemoryBarrierCount = count,
		        .pBufferMemoryBarriers = barriers,
		};
		vkCmdPipelineBarrier2(cmd, &depInfo);
	}

	// -- Transient heap -------------------------------------------------------

	void RenderGraphStorage::AllocateTransientHeap(VkDeviceSize requiredSize)
	{
		// Destroy old heap + virtual block if they exist.
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

		if (requiredSize == 0)
		{
			return;
		}

		const VkMemoryRequirements memReqs{
		        .size = requiredSize,
		        .alignment = kTransientHeapAlignment,
		        .memoryTypeBits = std::numeric_limits<std::uint32_t>::max(),
		};
		const VmaAllocationCreateInfo allocInfo{
		        .usage = VMA_MEMORY_USAGE_UNKNOWN,
		        .requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		};
		if (vmaAllocateMemory(m_allocator, &memReqs, &allocInfo, &m_transientHeapAllocation, nullptr) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "RenderGraph: failed to allocate {} byte transient heap for VRAM aliasing.", requiredSize);
			m_transientHeapAllocation = VK_NULL_HANDLE;
			return;
		}
		m_transientHeapCapacity = requiredSize;

		const VmaVirtualBlockCreateInfo blockInfo{
		        .size = requiredSize,
		};
		if (vmaCreateVirtualBlock(&blockInfo, &m_virtualBlock) != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "RenderGraph: failed to create VmaVirtualBlock.");
			vmaFreeMemory(m_allocator, m_transientHeapAllocation);
			m_transientHeapAllocation = VK_NULL_HANDLE;
			m_transientHeapCapacity = 0;
		}
	}

	// -- Two-pass transient heap preparation ----------------------------------

	void RenderGraphStorage::PrepareTransientAllocations(const FrameTarget& target)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			return;
		}

		// Reset all transient entries' heap state from previous frame.
		for (auto& entry: m_transientImages)
		{
			entry.fromHeap = false;
			entry.m_virtualAlloc = nullptr;
			entry.heapOffset = VK_WHOLE_SIZE;
			entry.memReqSize = 0;
			entry.memReqAlignment = 0;
		}
		for (auto& entry: m_transientBuffers)
		{
			entry.fromHeap = false;
			entry.m_virtualAlloc = nullptr;
			entry.heapOffset = VK_WHOLE_SIZE;
			entry.memReqSize = 0;
			entry.memReqAlignment = 0;
		}

		// Phase 1: Query memory requirements for all transient images.
		for (auto& entry: m_transientImages)
		{
			if (entry.image)
			{
				continue;
			}
			if (entry.format == VK_FORMAT_UNDEFINED || static_cast<std::uint32_t>(entry.usage) == 0)
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
			        .format = entry.format,
			        .extent = {entry.extent.width, entry.extent.height, 1u},
			        .mipLevels = 1,
			        .arrayLayers = 1,
			        .samples = VK_SAMPLE_COUNT_1_BIT,
			        .tiling = VK_IMAGE_TILING_OPTIMAL,
			        .usage = gpu::ToVk(entry.usage),
			        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			VkImage tempImage = VK_NULL_HANDLE;
			if (vkCreateImage(m_device, &tempInfo, nullptr, &tempImage) == VK_SUCCESS)
			{
				VkMemoryRequirements reqs{};
				vkGetImageMemoryRequirements(m_device, tempImage, &reqs);
				vkDestroyImage(m_device, tempImage, nullptr);
				entry.memReqSize = reqs.size;
				entry.memReqAlignment = reqs.alignment;
			}
		}

		// Phase 2: Query memory requirements for all transient buffers.
		for (auto& entry: m_transientBuffers)
		{
			if (entry.buffer)
			{
				continue;
			}
			if (entry.size == 0)
			{
				continue;
			}

			const VkBufferCreateInfo tempInfo{
			        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			        .size = entry.size,
			        .usage = entry.usage,
			};
			VkBuffer tempBuffer = VK_NULL_HANDLE;
			if (vkCreateBuffer(m_device, &tempInfo, nullptr, &tempBuffer) == VK_SUCCESS)
			{
				VkMemoryRequirements reqs{};
				vkGetBufferMemoryRequirements(m_device, tempBuffer, &reqs);
				vkDestroyBuffer(m_device, tempBuffer, nullptr);
				entry.memReqSize = reqs.size;
				entry.memReqAlignment = reqs.alignment;
			}
		}

		// Phase 3: Calculate total size needed.
		VkDeviceSize totalSize = 0;

		// Helper: accumulate aligned.
		auto accumulate = [](VkDeviceSize current, VkDeviceSize size, VkDeviceSize alignment) -> VkDeviceSize
		{
			if (size == 0)
			{
				return current;
			}
			const VkDeviceSize aligned = AlignUp(current, alignment);
			return aligned + size;
		};

		for (auto& entry: m_transientImages)
		{
			if (!entry.image && entry.memReqSize > 0)
			{
				totalSize = accumulate(totalSize, entry.memReqSize, entry.memReqAlignment);
			}
		}
		for (auto& entry: m_transientBuffers)
		{
			if (!entry.buffer && entry.memReqSize > 0)
			{
				totalSize = accumulate(totalSize, entry.memReqSize, entry.memReqAlignment);
			}
		}

		// Phase 4: Allocate heap and virtual block if needed.
		if (totalSize > m_transientHeapCapacity)
		{
			AllocateTransientHeap(totalSize);
		}
		else if (m_virtualBlock != VK_NULL_HANDLE)
		{
			vmaClearVirtualBlock(m_virtualBlock);
		}

		if (m_virtualBlock == VK_NULL_HANDLE)
		{
			return; // heap allocation failed; everything falls back to VMA
		}

		// Phase 5: Allocate virtual offsets for each resource from the virtual block.
		for (auto& entry: m_transientImages)
		{
			if (entry.image || entry.memReqSize == 0)
			{
				continue;
			}
			VmaVirtualAllocationCreateInfo allocInfo{
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

		for (auto& entry: m_transientBuffers)
		{
			if (entry.buffer || entry.memReqSize == 0)
			{
				continue;
			}
			VmaVirtualAllocationCreateInfo allocInfo{
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

		// Update heap stats.
		VmaStatistics blockStats{};
		vmaGetVirtualBlockStatistics(m_virtualBlock, &blockStats);
		m_lastFrameStats.heapCapacity = m_transientHeapCapacity;
		m_lastFrameStats.heapUsed = blockStats.allocationBytes;
		m_lastFrameStats.aliasedImageCount = 0;
		m_lastFrameStats.aliasedBufferCount = 0;
		for (auto& entry: m_transientImages)
		{
			if (entry.fromHeap)
			{
				m_lastFrameStats.aliasedImageCount++;
			}
		}
		for (auto& entry: m_transientBuffers)
		{
			if (entry.fromHeap)
			{
				m_lastFrameStats.aliasedBufferCount++;
			}
		}

		AE_VERBOSE(LogCategory::Vulkan,
		        "Transient heap: {:.1f} MB total, {:.1f} MB used, {} aliased images, {} aliased buffers, {} cache images",
		        static_cast<double>(m_lastFrameStats.heapCapacity) / (1024.0 * 1024.0),
		        static_cast<double>(m_lastFrameStats.heapUsed) / (1024.0 * 1024.0),
		        m_lastFrameStats.aliasedImageCount,
		        m_lastFrameStats.aliasedBufferCount,
		        m_lastFrameStats.cacheSize);
	}

	// -- EnsureTransientImages ------------------------------------------------

	void RenderGraphStorage::EnsureTransientImages(const FrameTarget& target)
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: transient images require Initialize(device, allocator).");
			return;
		}

		for (std::uint32_t idx = 0; idx < m_transientImages.size(); ++idx)
		{
			auto& entry = m_transientImages[idx];
			if (entry.image)
			{
				continue;
			}
			if (entry.format == VK_FORMAT_UNDEFINED || static_cast<std::uint32_t>(entry.usage) == 0)
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

			// Check cache first.
			const ImageCacheKey key = MakeCacheKey(entry, entry.extent);
			UniqueImage cached = TryPullFromCache(key);
			if (cached)
			{
				entry.image = std::move(cached);
				entry.allocatedExtent = entry.extent;
				m_lastFrameStats.transientCacheHit++;
#ifndef NDEBUG
				SetTrackedLayout(entry.image.Get(), VK_IMAGE_LAYOUT_UNDEFINED);
#endif
				continue;
			}

			// Try heap aliased allocation (pre-allocated by PrepareTransientAllocations).
			if (entry.fromHeap && m_transientHeapAllocation != VK_NULL_HANDLE && entry.heapOffset != VK_WHOLE_SIZE)
			{
				AE_EXPECT_OR_THROW(newImage,
				        UniqueImage::CreateAliased(m_device,
				                m_allocator,
				                {
				                        .extent = entry.extent,
				                        .format = gpu::FromVk(entry.format),
				                        .usage = entry.usage,
				                },
				                m_transientHeapAllocation,
				                entry.heapOffset));
				entry.image = std::move(newImage);
				entry.allocatedExtent = entry.extent;
				m_lastFrameStats.transientAllocated++;
				m_lastFrameStats.transientCacheMiss++;
				const std::string entryName = entry.bindlessRequested ? std::format("RenderGraph.Transient.Aliased.Bindless[{}]", idx) : std::format("RenderGraph.Transient.Aliased[{}]", idx);
				entry.image.SetName(m_device, entryName.c_str());
#ifndef NDEBUG
				SetTrackedLayout(entry.image.Get(), VK_IMAGE_LAYOUT_UNDEFINED);
#endif
				continue;
			}

			// Fallback: VMA-backed allocation.
			AE_EXPECT_OR_THROW(newImage,
			        UniqueImage::Create(m_device,
			                m_allocator,
			                {
			                        .extent = entry.extent,
			                        .format = gpu::FromVk(entry.format),
			                        .usage = entry.usage,
			                }));
			entry.image = std::move(newImage);
			entry.allocatedExtent = entry.extent;
			m_lastFrameStats.transientAllocated++;
			m_lastFrameStats.transientCacheMiss++;
			const std::string entryName = entry.bindlessRequested ? std::format("RenderGraph.Transient.Bindless[{}]", idx) : std::format("RenderGraph.Transient[{}]", idx);
			entry.image.SetName(m_device, entryName.c_str());
#ifndef NDEBUG
			SetTrackedLayout(entry.image.Get(), VK_IMAGE_LAYOUT_UNDEFINED);
#endif
		}

		EvictStaleCacheEntries();

		// Compute total cache occupancy.
		m_lastFrameStats.cacheSize = 0;
		for (const auto& [key, entries]: m_imageCache)
		{
			m_lastFrameStats.cacheSize += entries.size();
		}
	}

	// -- Transient buffers ----------------------------------------------------

	uint32_t RenderGraphStorage::AddTransientBufferSlot(VkDeviceSize size, VkBufferUsageFlags usage)
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
		m_transientBuffers.push_back(std::move(entry));
		return static_cast<uint32_t>(m_transientBuffers.size() - 1);
	}

	void RenderGraphStorage::EnsureTransientBuffers()
	{
		if (m_device == VK_NULL_HANDLE || m_allocator == VK_NULL_HANDLE)
		{
			AE_WARN(LogCategory::Engine, "RenderGraphStorage: transient buffers require Initialize(device, allocator).");
			return;
		}

		for (std::uint32_t idx = 0; idx < m_transientBuffers.size(); ++idx)
		{
			auto& entry = m_transientBuffers[idx];
			if (entry.buffer)
			{
				continue;
			}
			if (entry.size == 0)
			{
				continue;
			}

			// Try heap aliased allocation (pre-allocated by PrepareTransientAllocations).
			if (entry.fromHeap && m_transientHeapAllocation != VK_NULL_HANDLE && entry.heapOffset != VK_WHOLE_SIZE)
			{
				AE_EXPECT_OR_THROW(newBuffer, UniqueBuffer::CreateAliased(m_device, m_allocator, entry.size, entry.usage, m_transientHeapAllocation, entry.heapOffset));
				entry.buffer = std::move(newBuffer);
				m_lastFrameStats.transientAllocated++;
				const std::string entryName = std::format("RenderGraph.Transient.Buffer.Aliased[{}]", idx);
				entry.buffer.SetName(entryName.c_str());
				continue;
			}

			// Fallback: VMA-backed buffer.
			AE_EXPECT_OR_THROW(newBuffer, UniqueBuffer::CreateDeviceLocal(m_allocator, m_device, entry.size, entry.usage));
			entry.buffer = std::move(newBuffer);
			m_lastFrameStats.transientAllocated++;
			const std::string entryName = std::format("RenderGraph.Transient.Buffer[{}]", idx);
			entry.buffer.SetName(entryName.c_str());
		}
	}

	VkBuffer RenderGraphStorage::ResolveTransientBuffer(uint32_t idx) const
	{
		if (idx < m_transientBuffers.size())
		{
			return m_transientBuffers[idx].buffer.Get();
		}
		return VK_NULL_HANDLE;
	}

	bool RenderGraphStorage::IsTransientBufferSlotValid(uint32_t idx) const
	{
		if (idx >= m_transientBuffers.size())
		{
			return false;
		}
		return static_cast<bool>(m_transientBuffers[idx].buffer);
	}

	void RenderGraphStorage::ReleaseTransientBuffer(uint32_t idx)
	{
		if (idx >= m_transientBuffers.size())
		{
			return;
		}

		auto& entry = m_transientBuffers[idx];

		if (entry.fromHeap)
		{
			if (entry.m_virtualAlloc)
			{
				vmaVirtualFree(m_virtualBlock, entry.m_virtualAlloc);
				entry.m_virtualAlloc = nullptr;
			}
			entry.buffer.Reset();
		}
		else if (entry.buffer)
		{
			entry.buffer.Reset();
		}

		entry = {};
		m_freeTransientBufferSlots.push_back(idx);
	}
} // namespace aether
