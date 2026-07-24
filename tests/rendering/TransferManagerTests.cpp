// End-to-end coverage for the async upload path: TransferManager tickets, the timeline
// semaphore, deferred completion callbacks, and a real DMA copy verified by readback.
//
// Uses its own headless Vulkan device (no window/surface) so the suite stays runnable
// from a plain test harness. On machines with no Vulkan-capable device the whole suite
// is skipped rather than failed - the GPU boxes are where this coverage matters.

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <optional>

#include <VkBootstrap.h>

#include "gpu/CommandList.hpp"
#include "vulkan/TransferManager.hpp"
#include "vulkan/volk.hpp"

using namespace aether;

namespace
{
	// One headless device shared by every case in this TU (device creation is slow).
	struct HeadlessVulkan
	{
		vkb::Instance instance{};
		vkb::Device device{};
		VkQueue queue = VK_NULL_HANDLE;
		std::uint32_t queueFamily = 0;
		bool valid = false;

		HeadlessVulkan()
		{
			if (volkInitialize() != VK_SUCCESS)
			{
				return;
			}

			auto instanceResult = vkb::InstanceBuilder{}
			                              .set_app_name("AetherCore.TransferManagerTests")
			                              .set_headless(true)
			                              .require_api_version(1, 3, 0)
			                              .build();
			if (!instanceResult)
			{
				return;
			}
			instance = instanceResult.value();
			volkLoadInstance(instance.instance);

			VkPhysicalDeviceVulkan12Features features12{};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			features12.timelineSemaphore = VK_TRUE;
			VkPhysicalDeviceVulkan13Features features13{};
			features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			features13.synchronization2 = VK_TRUE;

			auto physicalResult = vkb::PhysicalDeviceSelector{instance}
			                              .set_minimum_version(1, 3)
			                              .set_required_features_12(features12)
			                              .set_required_features_13(features13)
			                              .defer_surface_initialization()
			                              .select();
			if (!physicalResult)
			{
				vkb::destroy_instance(instance);
				instance = {};
				return;
			}

			auto deviceResult = vkb::DeviceBuilder{physicalResult.value()}.build();
			if (!deviceResult)
			{
				vkb::destroy_instance(instance);
				instance = {};
				return;
			}
			device = deviceResult.value();
			volkLoadDevice(device.device);

			// Any transfer-capable queue works; graphics is universally transfer-capable.
			auto queueResult = device.get_queue(vkb::QueueType::graphics);
			auto familyResult = device.get_queue_index(vkb::QueueType::graphics);
			if (!queueResult || !familyResult)
			{
				vkb::destroy_device(device);
				vkb::destroy_instance(instance);
				device = {};
				instance = {};
				return;
			}
			queue = queueResult.value();
			queueFamily = familyResult.value();
			valid = true;
		}

		~HeadlessVulkan()
		{
			if (valid)
			{
				vkDeviceWaitIdle(device.device);
				vkb::destroy_device(device);
				vkb::destroy_instance(instance);
			}
		}
	};

	HeadlessVulkan& Gpu()
	{
		static HeadlessVulkan gpu;
		return gpu;
	}

	// Host-visible buffer with its own dedicated allocation - deliberately primitive so
	// the test depends on nothing but core Vulkan.
	struct HostBuffer
	{
		VkDevice device = VK_NULL_HANDLE;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		void* mapped = nullptr;

		static std::optional<HostBuffer> Create(const HeadlessVulkan& gpu, VkDeviceSize size, VkBufferUsageFlags usage)
		{
			HostBuffer out;
			out.device = gpu.device.device;

			const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
			if (vkCreateBuffer(out.device, &info, nullptr, &out.buffer) != VK_SUCCESS)
			{
				return std::nullopt;
			}

			VkMemoryRequirements requirements{};
			vkGetBufferMemoryRequirements(out.device, out.buffer, &requirements);

			VkPhysicalDeviceMemoryProperties memProps{};
			vkGetPhysicalDeviceMemoryProperties(gpu.device.physical_device, &memProps);
			std::uint32_t typeIndex = UINT32_MAX;
			constexpr VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
			for (std::uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
			{
				const bool allowed = (requirements.memoryTypeBits & (1u << i)) != 0;
				if (allowed && (memProps.memoryTypes[i].propertyFlags & wanted) == wanted)
				{
					typeIndex = i;
					break;
				}
			}
			if (typeIndex == UINT32_MAX)
			{
				vkDestroyBuffer(out.device, out.buffer, nullptr);
				return std::nullopt;
			}

			const VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = typeIndex};
			if (vkAllocateMemory(out.device, &alloc, nullptr, &out.memory) != VK_SUCCESS)
			{
				vkDestroyBuffer(out.device, out.buffer, nullptr);
				return std::nullopt;
			}
			vkBindBufferMemory(out.device, out.buffer, out.memory, 0);
			vkMapMemory(out.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
			return out;
		}

		void Destroy()
		{
			if (buffer != VK_NULL_HANDLE)
			{
				vkDestroyBuffer(device, buffer, nullptr);
				vkFreeMemory(device, memory, nullptr);
				buffer = VK_NULL_HANDLE;
				memory = VK_NULL_HANDLE;
				mapped = nullptr;
			}
		}
	};
} // namespace

TEST_CASE("TransferManager: ticket zero is always complete and waits are no-ops")
{
	if (!Gpu().valid)
	{
		return; // no Vulkan device on this machine - GPU coverage runs on dev boxes
	}

	vulkan::TransferManager transfer;
	transfer.Initialize(Gpu().device.device, Gpu().queue, Gpu().queueFamily, Gpu().queueFamily);

	CHECK(transfer.IsInitialized());
	CHECK(transfer.LastSubmitted() == 0);
	CHECK(transfer.IsComplete(0));
	transfer.WaitFor(0); // must not hang or crash

	transfer.Shutdown();
	CHECK_FALSE(transfer.IsInitialized());
}

TEST_CASE("TransferManager: submit copies data, signals the ticket, and runs completions")
{
	if (!Gpu().valid)
	{
		return;
	}

	auto src = HostBuffer::Create(Gpu(), 4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	auto dst = HostBuffer::Create(Gpu(), 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	REQUIRE(src.has_value());
	REQUIRE(dst.has_value());

	std::uint8_t pattern[4096];
	for (std::size_t i = 0; i < sizeof(pattern); ++i)
	{
		pattern[i] = static_cast<std::uint8_t>((i * 31 + 7) & 0xFF);
	}
	std::memcpy(src->mapped, pattern, sizeof(pattern));
	std::memset(dst->mapped, 0, sizeof(pattern));

	vulkan::TransferManager transfer;
	transfer.Initialize(Gpu().device.device, Gpu().queue, Gpu().queueFamily, Gpu().queueFamily);

	std::atomic<bool> completed{false};
	const auto ticket = transfer.Submit(
	        [&](gpu::CommandList& cmd) { cmd.CopyBuffer(static_cast<void*>(src->buffer), static_cast<void*>(dst->buffer), 0, 0, sizeof(pattern)); },
	        [&]() { completed.store(true, std::memory_order_release); });

	CHECK(ticket == 1);
	CHECK(transfer.LastSubmitted() == ticket);

	transfer.WaitFor(ticket);
	CHECK(transfer.IsComplete(ticket));
	CHECK(completed.load(std::memory_order_acquire)); // WaitFor reclaims finished submissions
	CHECK(std::memcmp(dst->mapped, pattern, sizeof(pattern)) == 0);

	transfer.Shutdown();
	src->Destroy();
	dst->Destroy();
}

TEST_CASE("TransferManager: tickets are monotonic and shutdown drains in-flight work")
{
	if (!Gpu().valid)
	{
		return;
	}

	auto src = HostBuffer::Create(Gpu(), 256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	auto dst = HostBuffer::Create(Gpu(), 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	REQUIRE(src.has_value());
	REQUIRE(dst.has_value());
	std::memset(src->mapped, 0xAB, 256);

	vulkan::TransferManager transfer;
	transfer.Initialize(Gpu().device.device, Gpu().queue, Gpu().queueFamily, Gpu().queueFamily);

	std::atomic<int> completions{0};
	vulkan::TransferManager::Ticket last = 0;
	for (int i = 0; i < 8; ++i)
	{
		const auto ticket = transfer.Submit(
		        [&](gpu::CommandList& cmd) { cmd.CopyBuffer(static_cast<void*>(src->buffer), static_cast<void*>(dst->buffer), 0, 0, 256); },
		        [&]() { completions.fetch_add(1, std::memory_order_acq_rel); });
		CHECK(ticket == last + 1);
		last = ticket;
	}
	CHECK(transfer.LastSubmitted() == 8);

	// Shutdown must wait for everything and run every completion callback.
	transfer.Shutdown();
	CHECK(completions.load(std::memory_order_acquire) == 8);

	src->Destroy();
	dst->Destroy();
}
