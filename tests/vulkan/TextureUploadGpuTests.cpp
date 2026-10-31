#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/OneShotCmd.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "gpu/TextureUpload.hpp"
#include "platform/Window.hpp"
#include "vulkan/ResourceRegistry.hpp"
#include "vulkan/GpuEnumConversions.hpp"
#include "vulkan/VulkanContext.hpp"

namespace
{
	bool GpuUploadTestsEnabled()
	{
		const char* value = std::getenv("AETHER_TEST_GPU_UPLOADS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}

	struct ValidationCounts
	{
		std::atomic<unsigned> warnings = 0;
		std::atomic<unsigned> errors = 0;
	};

	VKAPI_ATTR VkBool32 VKAPI_CALL CountValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
	        const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
	{
		auto& counts = *static_cast<ValidationCounts*>(userData);
		if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
		{
			++counts.errors;
		}
		else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
		{
			++counts.warnings;
		}
		(void) data; // The context's existing messenger records the full diagnostic.
		return VK_FALSE;
	}

	struct UploadFixture
	{
		aether::Window window{"Texture upload regression", 320, 240, aether::Window::Mode::Windowed, true};
		std::unique_ptr<aether::VulkanContext> context;
		aether::ResourceRegistry registry;
		VkCommandPool pool = VK_NULL_HANDLE;
		ValidationCounts validation;
		VkDebugUtilsMessengerEXT observer = VK_NULL_HANDLE;

		UploadFixture()
		{
			auto result = aether::VulkanContext::Create(window, "Texture upload regression", false, true);
			if (!result)
			{
				throw std::runtime_error(result.error().message);
			}
			context = std::move(*result);
			registry.Init(context->GetDevice().device, context->GetAllocator());
			aether::gpu::ResourceRegistry::Initialize({.vulkanDevice = context->GetDevice().device, .vmaAllocator = context->GetAllocator(), .backendRegistry = &registry});
			const VkCommandPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = context->GetGraphicsQueueFamily()};
			REQUIRE(vkCreateCommandPool(context->GetDevice().device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
			const VkDebugUtilsMessengerCreateInfoEXT observerInfo{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
			        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
			        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
			        .pfnUserCallback = &CountValidation, .pUserData = &validation};
			REQUIRE(vkCreateDebugUtilsMessengerEXT(context->GetInstance().instance, &observerInfo, nullptr, &observer) == VK_SUCCESS);
		}

		~UploadFixture()
		{
			if (context)
			{
				vkDeviceWaitIdle(context->GetDevice().device);
				registry.Shutdown();
				if (pool != VK_NULL_HANDLE)
				{
					vkDestroyCommandPool(context->GetDevice().device, pool, nullptr);
				}
				if (observer != VK_NULL_HANDLE)
				{
					vkDestroyDebugUtilsMessengerEXT(context->GetInstance().instance, observer, nullptr);
				}
			}
		}
	};

	struct MipPayload
	{
		std::vector<std::byte> bytes;
		std::vector<aether::gpu::ImageUploadRegion> regions;
		std::vector<std::size_t> sizes;
	};

	MipPayload MakeMipPayload(std::uint32_t blockWidth, std::uint32_t blockBytes)
	{
		MipPayload payload;
		std::uint32_t width = 7, height = 5;
		for (std::uint32_t mip = 0; mip < 3; ++mip)
		{
			const std::size_t offset = (payload.bytes.size() + blockBytes - 1) / blockBytes * blockBytes;
			const std::size_t size = std::size_t((width + blockWidth - 1) / blockWidth) * ((height + blockWidth - 1) / blockWidth) * blockBytes;
			payload.bytes.resize(offset + size, std::byte{0xCC});
			for (std::size_t i = 0; i < size; ++i)
			{
				payload.bytes[offset + i] = static_cast<std::byte>((i * 17 + mip * 53 + 11) & 0xFF);
			}
			payload.regions.push_back({.offset = offset, .width = width, .height = height, .mipLevel = mip});
			payload.sizes.push_back(size);
			width = std::max(1u, width / 2);
			height = std::max(1u, height / 2);
		}
		return payload;
	}

	void CheckUploadReadback(UploadFixture& fixture, aether::gpu::Format format, const MipPayload& payload, bool useHostCopy)
	{
		using namespace aether;
		CAPTURE(format);
		CAPTURE(useHostCopy);
		const auto usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::TransferSrc | gpu::ImageUsage::Sampled;
		const gpu::TextureDesc desc{.format = format, .extent = {7, 5},
		        .usage = useHostCopy ? usage | gpu::ImageUsage::HostTransfer : usage,
		        .aspect = gpu::ImageAspect::Color, .mipLevels = 3, .debugName = "Texture upload GPU regression"};
		const auto texture = gpu::ResourceRegistry::CreateTexture(desc);
		REQUIRE(texture.IsValid());
		const auto uploaded = gpu::UploadTexture(fixture.context->GetDevice().device, fixture.context->GetGraphicsQueue(), fixture.pool,
		        texture, payload.bytes, payload.regions, useHostCopy);
		INFO((uploaded ? "upload succeeded" : uploaded.error().message));
		REQUIRE(uploaded.has_value());

		const auto readback = gpu::ResourceRegistry::CreateMappedBuffer({.size = payload.bytes.size(), .usage = gpu::BufferUsage::TransferDst,
		        .memoryUsage = gpu::MappedMemoryUsage::GpuToCpu, .debugName = "Texture upload regression readback"});
		REQUIRE(readback.IsValid());
		const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(readback);
		REQUIRE(mapped.mappedPtr != nullptr);
		const auto image = static_cast<VkImage>(gpu::ResourceRegistry::ResolveTextureImage(texture));
		gpu::OneShotCmd commands;
		REQUIRE(commands.Begin(fixture.context->GetDevice().device, fixture.pool));
		const auto cmd = static_cast<VkCommandBuffer>(commands.CmdList().GetCommandBuffer());
		const VkImageMemoryBarrier2 toCopy{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
		        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		        .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
		        .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
		        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 3, .baseArrayLayer = 0, .layerCount = 1}};
		const VkDependencyInfo beforeCopy{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &toCopy};
		vkCmdPipelineBarrier2(cmd, &beforeCopy);
		std::vector<VkBufferImageCopy> regions;
		for (const auto& mip: payload.regions)
		{
			regions.push_back({.bufferOffset = mip.offset, .bufferRowLength = 0, .bufferImageHeight = 0,
			        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip.mipLevel, .baseArrayLayer = 0, .layerCount = 1},
			        .imageOffset = {0, 0, 0}, .imageExtent = {mip.width, mip.height, 1}});
		}
		const auto buffer = static_cast<VkBuffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(readback));
		vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, static_cast<std::uint32_t>(regions.size()), regions.data());
		const VkBufferMemoryBarrier2 toHost{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
		        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
		        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .buffer = buffer, .offset = 0, .size = VK_WHOLE_SIZE};
		const VkDependencyInfo afterCopy{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .bufferMemoryBarrierCount = 1, .pBufferMemoryBarriers = &toHost};
		vkCmdPipelineBarrier2(cmd, &afterCopy);
		REQUIRE(commands.EndAndSubmit(fixture.context->GetGraphicsQueue()));
		gpu::ResourceRegistry::InvalidateMappedBuffer(readback, 0, payload.bytes.size());
		const auto* actual = static_cast<const std::byte*>(mapped.mappedPtr);
		for (std::size_t mip = 0; mip < payload.regions.size(); ++mip)
		{
			CAPTURE(mip);
			const auto offset = payload.regions[mip].offset;
			CHECK(std::memcmp(actual + offset, payload.bytes.data() + offset, payload.sizes[mip]) == 0);
		}
		gpu::ResourceRegistry::Destroy(readback);
		gpu::ResourceRegistry::Destroy(texture);
		gpu::ResourceRegistry::DrainPendingDestructions();
	}
}

// This case requires a display and real Vulkan device. Run it alone in a fresh process
// with AETHER_TEST_GPU_UPLOADS=1; the fixture owns the GPU facade's process-global registry.
TEST_CASE("GPU texture uploads preserve odd-sized mip chains through staging and host copy" * doctest::skip(!GpuUploadTestsEnabled()))
{
	UploadFixture fixture;
	struct FormatCase
	{
		aether::gpu::Format format;
		std::uint32_t blockWidth;
		std::uint32_t blockBytes;
		const char* name;
	};
	const FormatCase formats[] = {{aether::gpu::Format::R8G8B8A8Unorm, 1, 4, "RGBA8"}, {aether::gpu::Format::R32G32B32A32Sfloat, 1, 16, "RGBA32F font curves"},
	        {aether::gpu::Format::BC4UnormBlock, 4, 8, "BC4"}, {aether::gpu::Format::BC7UnormBlock, 4, 16, "BC7"}};
	for (const auto& format: formats)
	{
		const auto payload = MakeMipPayload(format.blockWidth, format.blockBytes);
		std::cout << "GPU upload readback: " << format.name << ", staging, 7x5/3x2/1x1 mips\n";
		CheckUploadReadback(fixture, format.format, payload, false);
		const bool hostCopySupported = aether::gpu::CanUseHostImageCopy(format.format, {7, 5}, 3);
		if (const char* forcedOff = std::getenv("AETHER_DISABLE_HOST_IMAGE_COPY"); forcedOff != nullptr && std::strcmp(forcedOff, "1") == 0)
		{
			CHECK_FALSE(hostCopySupported);
		}
		// Production uploads require Sampled|TransferDst|HostTransfer. The observer also
		// needs TransferSrc for readback, so independently check that larger usage combination.
		VkImageFormatProperties hostReadbackProperties{};
		const bool canReadHostImage = hostCopySupported
		        && vkGetPhysicalDeviceImageFormatProperties(fixture.context->GetPhysicalDevice(), aether::gpu::ToVk(format.format), VK_IMAGE_TYPE_2D,
		                   VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		                   0, &hostReadbackProperties) == VK_SUCCESS;
		if (canReadHostImage)
		{
			std::cout << "GPU upload readback: " << format.name << ", host copy, 7x5/3x2/1x1 mips\n";
			CheckUploadReadback(fixture, format.format, payload, true);
		}
		else
		{
			std::cout << "GPU upload readback: " << format.name << ", host-copy readback unavailable (staging verified)\n";
		}
	}
	REQUIRE(fixture.context->WaitIdle().has_value());
	fixture.registry.Shutdown();
	CHECK(fixture.validation.errors.load() == 0);
	CHECK(fixture.validation.warnings.load() == 0);
}
