#include "gpu/UploadContext.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>
#include "gpu/ResourceRegistry.hpp"

#include "gpu/CommandList.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/TransferManager.hpp"
#include "vulkan/QueueSubmit.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>

namespace aether::gpu
{
	namespace
	{
		void RetireImageStaging(BufferHandle staging, std::function<void()> callback)
		{
			const std::lock_guard lock(ResourceRegistry::BufferMutex());
			ResourceRegistry::Destroy(staging);
			if (callback) ResourceRegistry::DeferDestructionCallback(std::move(callback));
		}
	}

	struct Impl
	{
		VkDevice device = VK_NULL_HANDLE;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkCommandPool imageCommandPool = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		aether::ResourceRegistry* backendRegistry = nullptr;
		aether::vulkan::TransferManager* transfer = nullptr;
		struct ImageUpload
		{
			std::uint64_t ticket;
			VkCommandBuffer command;
			VkFence fence;
			BufferHandle staging;
			std::function<void()> onStagingRetired;
		};
		// Render-thread grass collection and game-thread texture streaming share
		// these records; the image pool is private to the same lock.
		mutable std::mutex uploadsMutex;
		std::vector<ImageUpload> images;
		std::uint64_t nextImageTicket = 1;
		std::uint64_t completedImageTicket = 0;
		struct BufferUpload
		{
			std::uint64_t ticket;
			BufferHandle staging;
			std::function<void()> onStagingRetired;
		};
		std::vector<BufferUpload> staging;
	};

	UploadContext::~UploadContext()
	{
		Destroy();
	}

	UploadContext::UploadContext(UploadContext&& other) noexcept
	      : m_impl(std::exchange(other.m_impl, nullptr))
	{
	}

	UploadContext& UploadContext::operator=(UploadContext&& other) noexcept
	{
		if (this != &other)
		{
			Destroy();
			m_impl = std::exchange(other.m_impl, nullptr);
		}
		return *this;
	}

	UploadContext UploadContext::Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry, void* transferManager)
	{
		AE_PROFILE_ZONE();
		auto* vkDevice = static_cast<VkDevice>(device);

		const VkCommandPoolCreateInfo poolInfo{
		        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		        .queueFamilyIndex = queueFamilyIndex,
		};

		VkCommandPool pool = VK_NULL_HANDLE;
		const VkResult result = vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool);
		if (result != VK_SUCCESS)
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::Create: vkCreateCommandPool failed (VkResult={}).", static_cast<int32_t>(result));
			return {};
		}
		VkCommandPool imagePool = VK_NULL_HANDLE;
		if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &imagePool) != VK_SUCCESS)
		{
			vkDestroyCommandPool(vkDevice, pool, nullptr);
			return {};
		}


		Impl* impl = new Impl{};
		impl->device = vkDevice;
		impl->commandPool = pool;
		impl->imageCommandPool = imagePool;
		impl->queue = static_cast<VkQueue>(queue);
		impl->backendRegistry = static_cast<aether::ResourceRegistry*>(backendRegistry);
		impl->transfer = static_cast<aether::vulkan::TransferManager*>(transferManager);

		UploadContext ctx;
		ctx.m_impl = impl;
		return ctx;
	}

	void UploadContext::Destroy()
	{
		if (m_impl == nullptr)
		{
			return;
		}

		Impl* impl = static_cast<Impl*>(m_impl);
		for (auto& image : impl->images)
		{
			vkWaitForFences(impl->device, 1, &image.fence, VK_TRUE, UINT64_MAX);
			vkDestroyFence(impl->device, image.fence, nullptr);
			vkFreeCommandBuffers(impl->device, impl->imageCommandPool, 1, &image.command);
			RetireImageStaging(image.staging, std::move(image.onStagingRetired));
		}
		for (auto& upload : impl->staging)
		{
			impl->transfer->WaitFor(upload.ticket);
			RetireImageStaging(upload.staging, std::move(upload.onStagingRetired));
		}
		vkDestroyCommandPool(impl->device, impl->imageCommandPool, nullptr);
		if (impl->device != VK_NULL_HANDLE && impl->commandPool != VK_NULL_HANDLE)
		{
			vkDestroyCommandPool(impl->device, impl->commandPool, nullptr);
		}

		delete impl;
		m_impl = nullptr;
	}

	void UploadContext::CopyBuffer(BufferHandle src, BufferHandle dst, DeviceSize size)
	{
		if (m_impl == nullptr)
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffer called on invalid context.");
			return;
		}

		const Impl* impl = static_cast<Impl*>(m_impl);
		AE_ASSERT(impl->backendRegistry != nullptr && impl->transfer != nullptr, "UploadContext: backendRegistry or transfer manager is null.");

		const VkBuffer srcBuffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(src));
		const VkBuffer dstBuffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(dst));
		if (srcBuffer == VK_NULL_HANDLE || dstBuffer == VK_NULL_HANDLE)
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffer: failed to resolve src or dst buffer.");
			return;
		}

		// On the TransferManager's queue (a dedicated DMA queue where the hardware has one),
		// not the graphics queue: a one-shot there queued behind the frame in flight and
		// cost ~3.7 ms per copy, which made a level's mesh uploads take seconds. The wait
		// keeps this call blocking so callers can free the staging buffer straight after;
		// the frame's submission waits on the transfer timeline, which publishes the copy
		// to rendering (see TransferManager).
		const aether::vulkan::TransferManager::Ticket ticket = impl->transfer->Submit([srcBuffer, dstBuffer, size](CommandList& cmdList) { cmdList.CopyBuffer(static_cast<void*>(srcBuffer), static_cast<void*>(dstBuffer), 0, 0, size); });
		impl->transfer->WaitFor(ticket);
	}

	std::uint64_t UploadContext::CopyBufferAsyncOwned(BufferHandle src, BufferHandle dst, DeviceSize size, DeviceSize destinationOffset, std::function<void()> onStagingRetired)
	{
		auto* impl = static_cast<Impl*>(m_impl);
		const VkBuffer srcBuffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(src));
		const VkBuffer dstBuffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(dst));
		if (srcBuffer == VK_NULL_HANDLE || dstBuffer == VK_NULL_HANDLE)
		{
			RetireImageStaging(src, std::move(onStagingRetired));
			return 0;
		}
		std::unique_lock lock(impl->uploadsMutex);
		std::uint64_t ticket = 0;
		try
		{
			if (impl->staging.size() == impl->staging.capacity())
			{
				impl->staging.reserve(std::max<std::size_t>(8, impl->staging.capacity() * 2));
			}
			ticket = impl->transfer->Submit([srcBuffer, dstBuffer, size, destinationOffset](CommandList& cmd) { cmd.CopyBuffer(static_cast<void*>(srcBuffer), static_cast<void*>(dstBuffer), 0, destinationOffset, size); }, {}, false);
		}
		catch (...)
		{
			lock.unlock();
			RetireImageStaging(src, std::move(onStagingRetired));
			throw;
		}
		if (!ticket)
		{
			lock.unlock();
			RetireImageStaging(src, std::move(onStagingRetired));
			return 0;
		}
		impl->staging.push_back({ticket, src, std::move(onStagingRetired)});
		return ticket;
	}

	std::uint64_t UploadContext::CopyImageAsyncOwned(BufferHandle staging, TextureHandle destination, std::span<const ImageUploadRegion> regions, std::function<void()> onStagingRetired)
	{
		std::vector<ImageUploadRegion> mips(regions.begin(), regions.end());
		for (std::uint32_t i = 0; i < mips.size(); ++i) mips[i].mipLevel = i;
		return CopyImageChunkAsyncOwned(staging, destination, mips, static_cast<std::uint32_t>(mips.size()), true, true, std::move(onStagingRetired));
	}

	std::uint64_t UploadContext::CopyImageChunkAsyncOwned(BufferHandle staging, TextureHandle destination, std::span<const ImageUploadRegion> regions, std::uint32_t mipLevels, bool first, bool last, std::function<void()> onStagingRetired)
	{
		auto* impl = static_cast<Impl*>(m_impl);
		const auto buffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(staging));
		const auto image = static_cast<VkImage>(ResourceRegistry::ResolveTextureImage(destination));
		if (!impl || !buffer || !image || regions.empty())
		{
			RetireImageStaging(staging, std::move(onStagingRetired));
			return 0;
		}
		std::vector<VkBufferImageCopy> copies;
		try
		{
			copies.reserve(regions.size());
			for (const auto& region : regions)
				copies.push_back(VkBufferImageCopy{.bufferOffset = region.offset, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mipLevel, 0, 1}, .imageOffset = {region.x, region.y, 0}, .imageExtent = {region.width, region.height, 1}});
		}
		catch (...) { RetireImageStaging(staging, std::move(onStagingRetired)); throw; }
		std::unique_lock lock(impl->uploadsMutex);
		try
		{
			if (impl->images.size() == impl->images.capacity())
			{
				impl->images.reserve(std::max<std::size_t>(8, impl->images.capacity() * 2));
			}
		}
		catch (...) { lock.unlock(); RetireImageStaging(staging, std::move(onStagingRetired)); throw; }
		VkCommandBufferAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = impl->imageCommandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
		VkCommandBuffer command = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(impl->device, &allocation, &command) != VK_SUCCESS)
		{
			lock.unlock();
			RetireImageStaging(staging, std::move(onStagingRetired));
			return 0;
		}
		VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vkCreateFence(impl->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS || vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
		{
			if (fence) vkDestroyFence(impl->device, fence, nullptr);
			vkFreeCommandBuffers(impl->device, impl->imageCommandPool, 1, &command);
			lock.unlock();
			RetireImageStaging(staging, std::move(onStagingRetired));
			return 0;
		}
		VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1}};
		if (first) vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		vkCmdCopyBufferToImage(command, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(copies.size()), copies.data());
		if (last)
		{
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		VkResult result = vkEndCommandBuffer(command);
		if (result == VK_SUCCESS)
		{
			VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
			const std::lock_guard lock(aether::vulkan::QueueSubmitMutex());
			result = vkQueueSubmit(impl->queue, 1, &submit, fence);
		}
		if (result != VK_SUCCESS)
		{
			vkDestroyFence(impl->device, fence, nullptr);
			vkFreeCommandBuffers(impl->device, impl->imageCommandPool, 1, &command);
			lock.unlock();
			RetireImageStaging(staging, std::move(onStagingRetired));
			return 0;
		}
		const std::uint64_t ticket = (std::uint64_t{1} << 63) | impl->nextImageTicket++;
		impl->images.push_back({ticket, command, fence, staging, std::move(onStagingRetired)});
		return ticket;
	}

	void UploadContext::CollectAsyncUploads()
	{
		auto* impl = static_cast<Impl*>(m_impl);
		std::unique_lock lock(impl->uploadsMutex);
		for (auto it = impl->images.begin(); it != impl->images.end();)
		{
			if (vkGetFenceStatus(impl->device, it->fence) != VK_SUCCESS) { ++it; continue; }
			impl->completedImageTicket = std::max(impl->completedImageTicket, it->ticket);
			vkDestroyFence(impl->device, it->fence, nullptr);
			vkFreeCommandBuffers(impl->device, impl->imageCommandPool, 1, &it->command);
			auto staging = it->staging;
			auto callback = std::move(it->onStagingRetired);
			it = impl->images.erase(it);
			lock.unlock();
			RetireImageStaging(staging, std::move(callback));
			lock.lock();
			it = impl->images.begin();
		}
		for (auto it = impl->staging.begin(); it != impl->staging.end();)
		{
			if (!impl->transfer->IsComplete(it->ticket)) { ++it; continue; }
			auto staging = it->staging;
			auto callback = std::move(it->onStagingRetired);
			it = impl->staging.erase(it);
			lock.unlock();
			RetireImageStaging(staging, std::move(callback));
			lock.lock();
			it = impl->staging.begin();
		}
	}

	bool UploadContext::IsUploadComplete(std::uint64_t ticket) const
	{
		const auto* impl = static_cast<const Impl*>(m_impl);
		const std::lock_guard lock(impl->uploadsMutex);
		if (ticket & (std::uint64_t{1} << 63))
		{
			if (ticket <= impl->completedImageTicket) return true;
			for (const auto& image : impl->images)
				if (image.ticket == ticket) return vkGetFenceStatus(impl->device, image.fence) == VK_SUCCESS;
			return false;
		}
		return impl->transfer->IsComplete(ticket);
	}

	void UploadContext::WaitForUpload(std::uint64_t ticket)
	{
		auto* impl = static_cast<Impl*>(m_impl);
		const std::lock_guard lock(impl->uploadsMutex);
		if (ticket & (std::uint64_t{1} << 63))
		{
			for (const auto& image : impl->images)
				if (image.ticket == ticket) { vkWaitForFences(impl->device, 1, &image.fence, VK_TRUE, UINT64_MAX); return; }
			return;
		}
		impl->transfer->WaitFor(ticket);
	}

	void* UploadContext::GetCommandPool() const
	{
		if (m_impl == nullptr)
		{
			return nullptr;
		}
		const Impl* impl = static_cast<Impl*>(m_impl);
		return static_cast<void*>(impl->commandPool);
	}
} // namespace aether::gpu
