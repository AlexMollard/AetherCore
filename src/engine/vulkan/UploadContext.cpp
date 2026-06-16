#include "gpu/UploadContext.hpp"

#include <utility>

#include "gpu/CommandList.hpp"
#include "gpu/OneShotCmd.hpp"
#include "utils/Assert.hpp"
#include "utils/Logger.hpp"
#include "vulkan/ResourceRegistry.hpp"
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

	UploadContext UploadContext::Create(Device device, std::uint32_t queueFamilyIndex, Queue queue, void* backendRegistry)
	{
		const auto vkDevice = static_cast<VkDevice>(device);

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

		Impl* impl = static_cast<Impl*>(m_impl);
		AE_ASSERT(impl->backendRegistry != nullptr, "UploadContext: backendRegistry is null.");

		const auto* srcEntry = impl->backendRegistry->Resolve(src);
		const auto* dstEntry = impl->backendRegistry->Resolve(dst);
		if (srcEntry == nullptr || dstEntry == nullptr)
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffer: failed to resolve src or dst buffer.");
			return;
		}

		OneShotCmd cmd;
		if (!cmd.Begin(static_cast<void*>(impl->device), static_cast<void*>(impl->commandPool)))
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffer: failed to begin OneShotCmd.");
			return;
		}

		cmd.CmdList().CopyBuffer(static_cast<void*>(srcEntry->buffer), static_cast<void*>(dstEntry->buffer), 0, 0, size);

		if (!cmd.EndAndSubmit(static_cast<void*>(impl->queue)))
		{
			AE_ERROR(LogCategory::Vulkan, "UploadContext::CopyBuffer: failed to submit OneShotCmd.");
		}
	}

	void* UploadContext::GetCommandPool() const
	{
		if (m_impl == nullptr)
		{
			return nullptr;
		}
		Impl* impl = static_cast<Impl*>(m_impl);
		return static_cast<void*>(impl->commandPool);
	}
} // namespace aether::gpu
