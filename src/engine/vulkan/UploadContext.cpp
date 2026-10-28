#include "gpu/UploadContext.hpp"

#include <algorithm>
#include <utility>
#include <vector>
#include "gpu/ResourceRegistry.hpp"

#include "gpu/CommandList.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/TransferManager.hpp"
#include "vulkan/volk.hpp"
#include <vk_mem_alloc.h>

namespace aether::gpu
{
	struct Impl
	{
		VkDevice device = VK_NULL_HANDLE;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		aether::ResourceRegistry* backendRegistry = nullptr;
		aether::vulkan::TransferManager* transfer = nullptr;
		std::vector<std::pair<std::uint64_t, BufferHandle>> staging;
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

		Impl* impl = new Impl{};
		impl->device = vkDevice;
		impl->commandPool = pool;
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

		const Impl* impl = static_cast<Impl*>(m_impl);
		for (const auto& [ticket, handle]: impl->staging)
		{
			impl->transfer->WaitFor(ticket);
			ResourceRegistry::Destroy(handle);
		}
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

	std::uint64_t UploadContext::CopyBufferAsyncOwned(BufferHandle src, BufferHandle dst, DeviceSize size)
	{
		auto* impl = static_cast<Impl*>(m_impl);
		const VkBuffer srcBuffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(src));
		const VkBuffer dstBuffer = static_cast<VkBuffer>(ResourceRegistry::ResolveBufferVkHandle(dst));
		if (srcBuffer == VK_NULL_HANDLE || dstBuffer == VK_NULL_HANDLE)
		{
			ResourceRegistry::Destroy(src);
			return 0;
		}
		const auto ticket = impl->transfer->Submit([srcBuffer, dstBuffer, size](CommandList& cmd) { cmd.CopyBuffer(static_cast<void*>(srcBuffer), static_cast<void*>(dstBuffer), 0, 0, size); }, {}, false);
		impl->staging.emplace_back(ticket, src);
		return ticket;
	}

	void UploadContext::CollectAsyncUploads()
	{
		auto* impl = static_cast<Impl*>(m_impl);
		std::erase_if(impl->staging, [&](const auto& item)
		{
			if (!impl->transfer->IsComplete(item.first)) return false;
			ResourceRegistry::Destroy(item.second);
			return true;
		});
	}

	bool UploadContext::IsUploadComplete(std::uint64_t ticket) const
	{
		return static_cast<const Impl*>(m_impl)->transfer->IsComplete(ticket);
	}

	void UploadContext::WaitForUpload(std::uint64_t ticket)
	{
		static_cast<Impl*>(m_impl)->transfer->WaitFor(ticket);
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
