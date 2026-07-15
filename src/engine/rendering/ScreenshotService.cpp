#include "rendering/ScreenshotService.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

#include "vulkan/volk.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "gpu/GpuDeviceFactory.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "utils/Logger.hpp"

namespace aether
{
	namespace
	{
		// magnitude smaller on disk and viewable/inline-loadable everywhere, so the
		bool WritePng(const std::string& path, const std::uint8_t* rgba, std::uint32_t w, std::uint32_t h)
		{
			std::error_code ec;
			std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
			return stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba, static_cast<int>(w * 4u)) != 0;
		}

		std::uint32_t BytesPerPixel(gpu::Format f)
		{
			switch (f)
			{
				case gpu::Format::R8Unorm:
					return 1;
				case gpu::Format::R8G8B8A8Unorm:
				case gpu::Format::R8G8B8A8Srgb:
				case gpu::Format::B8G8R8A8Unorm:
				case gpu::Format::B8G8R8A8Srgb:
				case gpu::Format::D32Sfloat:
					return 4;
				case gpu::Format::R16G16B16A16Sfloat:
				case gpu::Format::R32G32Sfloat:
					return 8;
				case gpu::Format::Undefined:
				case gpu::Format::R32G32B32Sfloat:
				case gpu::Format::R32G32B32A32Sfloat:
				case gpu::Format::D16Unorm:
				case gpu::Format::D24UnormS8Uint:
				case gpu::Format::X8D24UnormPack32:
				case gpu::Format::D16UnormS8Uint:
				case gpu::Format::D32SfloatS8Uint:
				case gpu::Format::BC4UnormBlock:
				case gpu::Format::BC7UnormBlock:
				case gpu::Format::BC7SrgbBlock:
				default:
					return 0;
			}
		}

		float HalfToFloat(std::uint16_t h)
		{
			const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
			std::uint32_t exp = (h >> 10) & 0x1Fu;
			std::uint32_t mant = h & 0x3FFu;
			std::uint32_t f = 0;
			if (exp == 0)
			{
				if (mant != 0)
				{
					exp = 1;
					while ((mant & 0x400u) == 0)
					{
						mant <<= 1;
						--exp;
					}
					mant &= 0x3FFu;
					f = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
				}
				else
				{
					f = sign;
				}
			}
			else if (exp == 0x1Fu)
			{
				f = sign | 0x7F800000u | (mant << 13);
			}
			else
			{
				f = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
			}
			float r = 0.0f;
			std::memcpy(&r, &f, 4);
			return r;
		}

		std::vector<std::uint8_t> ConvertToRgba(gpu::Format f, const std::uint8_t* src, std::uint32_t w, std::uint32_t h)
		{
			const std::size_t n = static_cast<std::size_t>(w) * h;
			std::vector<std::uint8_t> out(n * 4u, 255u);
			const auto gray = [&](std::size_t i, std::uint8_t g)
			{
				out[i * 4] = g;
				out[i * 4 + 1] = g;
				out[i * 4 + 2] = g;
			};
			switch (f)
			{
				case gpu::Format::R8G8B8A8Unorm:
				case gpu::Format::R8G8B8A8Srgb:
					std::memcpy(out.data(), src, n * 4u);
					break;
				case gpu::Format::B8G8R8A8Unorm:
				case gpu::Format::B8G8R8A8Srgb:
					for (std::size_t i = 0; i < n; ++i)
					{
						out[i * 4] = src[i * 4 + 2];
						out[i * 4 + 1] = src[i * 4 + 1];
						out[i * 4 + 2] = src[i * 4];
						out[i * 4 + 3] = src[i * 4 + 3];
					}
					break;
				case gpu::Format::R8Unorm:
					for (std::size_t i = 0; i < n; ++i)
					{
						gray(i, src[i]);
					}
					break;
				case gpu::Format::D32Sfloat:
				{
					const auto* d = reinterpret_cast<const float*>(src);
					float mn = 1e30f;
					float mx = -1e30f;
					for (std::size_t i = 0; i < n; ++i)
					{
						if (d[i] < 1.0f)
						{
							mn = std::min(mn, d[i]);
							mx = std::max(mx, d[i]);
						}
					}
					const float range = mx > mn ? mx - mn : 1.0f;
					for (std::size_t i = 0; i < n; ++i)
					{
						gray(i, static_cast<std::uint8_t>(std::clamp((d[i] - mn) / range, 0.0f, 1.0f) * 255.0f));
					}
					break;
				}
				case gpu::Format::R32G32Sfloat:
				{
					const auto* p = reinterpret_cast<const float*>(src);
					float mn = 1e30f;
					float mx = -1e30f;
					for (std::size_t i = 0; i < n; ++i)
					{
						mn = std::min(mn, p[i * 2]);
						mx = std::max(mx, p[i * 2]);
					}
					const float range = mx > mn ? mx - mn : 1.0f;
					for (std::size_t i = 0; i < n; ++i)
					{
						gray(i, static_cast<std::uint8_t>(std::clamp((p[i * 2] - mn) / range, 0.0f, 1.0f) * 255.0f));
					}
					break;
				}
				case gpu::Format::R16G16B16A16Sfloat:
				{
					const auto* p = reinterpret_cast<const std::uint16_t*>(src);
					const auto tm = [](float c)
					{
						c = c / (1.0f + c);
						c = std::pow(std::clamp(c, 0.0f, 1.0f), 1.0f / 2.2f);
						return static_cast<std::uint8_t>(c * 255.0f);
					};
					for (std::size_t i = 0; i < n; ++i)
					{
						out[i * 4] = tm(HalfToFloat(p[i * 4]));
						out[i * 4 + 1] = tm(HalfToFloat(p[i * 4 + 1]));
						out[i * 4 + 2] = tm(HalfToFloat(p[i * 4 + 2]));
					}
					break;
				}
				case gpu::Format::Undefined:
				case gpu::Format::R32G32B32Sfloat:
				case gpu::Format::R32G32B32A32Sfloat:
				case gpu::Format::D16Unorm:
				case gpu::Format::D24UnormS8Uint:
				case gpu::Format::X8D24UnormPack32:
				case gpu::Format::D16UnormS8Uint:
				case gpu::Format::D32SfloatS8Uint:
				case gpu::Format::BC4UnormBlock:
				case gpu::Format::BC7UnormBlock:
				case gpu::Format::BC7SrgbBlock:
				default:
					return {};
			}
			return out;
		}

		VkImageLayout ToVkLayout(gpu::ImageLayout l)
		{
			switch (l)
			{
				case gpu::ImageLayout::General:
					return VK_IMAGE_LAYOUT_GENERAL;
				case gpu::ImageLayout::ColorAttachment:
					return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
				case gpu::ImageLayout::ShaderReadOnly:
					return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				case gpu::ImageLayout::TransferSrc:
					return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				case gpu::ImageLayout::TransferDst:
					return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				case gpu::ImageLayout::Undefined:
				case gpu::ImageLayout::DepthAttachment:
				default:
					return VK_IMAGE_LAYOUT_GENERAL;
			}
		}
	} // namespace

	void ScreenshotService::Init(gpu::Device device, std::uint32_t graphicsQueueFamily, gpu::Queue queue)
	{
		m_device = device;
		m_queue = queue;
		m_queueFamily = graphicsQueueFamily;
		m_pool = gpu::Factory::CreateCommandPool(device, gpu::Factory::CommandPoolDesc{.queueFamilyIndex = graphicsQueueFamily, .transient = true, .resetCommandBuffer = false});
	}

	void ScreenshotService::Shutdown()
	{
		if (m_pool != nullptr && m_device != nullptr)
		{
			vkDestroyCommandPool(static_cast<VkDevice>(m_device), static_cast<VkCommandPool>(m_pool), nullptr);
			m_pool = nullptr;
		}
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_pending)
		{
			m_pending->promise.set_value("");
			m_pending.reset();
		}
		m_device = nullptr;
	}

	std::future<std::string> ScreenshotService::Request(std::string absolutePath)
	{
		std::promise<std::string> promise;
		std::future<std::string> fut = promise.get_future();
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_pending)
		{
			m_pending->promise.set_value("");
		}
		m_pending = Pending{.path = std::move(absolutePath), .promise = std::move(promise)};
		return fut;
	}

	std::future<std::string> ScreenshotService::RequestImage(void* image, gpu::Extent2D extent, gpu::Format format, gpu::ImageAspect aspect, gpu::ImageLayout srcLayout, std::string absolutePath)
	{
		std::promise<std::string> promise;
		std::future<std::string> fut = promise.get_future();
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_pending)
		{
			m_pending->promise.set_value("");
		}
		m_pending = Pending{.path = std::move(absolutePath), .image = image, .extent = extent, .format = format, .aspect = aspect, .srcLayout = srcLayout, .promise = std::move(promise)};
		return fut;
	}

	void ScreenshotService::RecordFrameCapture(void* cmdV, void* imageV, gpu::Extent2D extent, gpu::Format format)
	{
		if (m_device == nullptr || cmdV == nullptr || imageV == nullptr)
		{
			return;
		}

		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_pending || m_pending->image != nullptr || m_frameCapture)
			{
				return;
			}
			m_frameCapture.emplace();
			m_frameCapture->path = std::move(m_pending->path);
			m_frameCapture->promise = std::move(m_pending->promise);
			m_pending.reset();
		}

		const std::uint32_t bpp = BytesPerPixel(format);
		const std::uint32_t width = extent.width;
		const std::uint32_t height = extent.height;
		if (bpp == 0 || width == 0 || height == 0)
		{
			AE_WARN(LogCategory::Render, "ScreenshotService: unsupported swapchain format {} for capture to '{}'.", static_cast<int>(format), m_frameCapture->path);
			m_frameCapture->promise.set_value(std::string{});
			m_frameCapture.reset();
			return;
		}
		const std::uint32_t byteSize = width * height * bpp;

		const gpu::MappedBufferDesc bufDesc{.size = byteSize, .usage = gpu::BufferUsage::TransferDst, .memoryUsage = gpu::MappedMemoryUsage::Auto, .debugName = "Screenshot"};
		const gpu::BufferHandle bufHandle = gpu::ResourceRegistry::CreateMappedBuffer(bufDesc);
		if (!bufHandle.IsValid())
		{
			m_frameCapture->promise.set_value(std::string{});
			m_frameCapture.reset();
			return;
		}

		auto* cmd = static_cast<VkCommandBuffer>(cmdV);
		auto* image = static_cast<VkImage>(imageV);
		auto* buffer = static_cast<VkBuffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(bufHandle));

		const auto barrier = [&](VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
		{
			const VkImageMemoryBarrier b{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask = srcA,
			        .dstAccessMask = dstA,
			        .oldLayout = oldL,
			        .newLayout = newL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image = image,
			        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
		};

		barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

		const VkBufferImageCopy region{
		        .bufferOffset = 0,
		        .bufferRowLength = 0,
		        .bufferImageHeight = 0,
		        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		        .imageOffset = {0, 0, 0},
		        .imageExtent = {width, height, 1},
		};
		vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

		// expected old layout.
		barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

		m_frameCapture->buffer = bufHandle;
		m_frameCapture->mapped = gpu::ResourceRegistry::ResolveMappedBuffer(bufHandle).mappedPtr;
		m_frameCapture->width = width;
		m_frameCapture->height = height;
		m_frameCapture->format = format;
	}

	void ScreenshotService::CompleteFrameCapture()
	{
		if (!m_frameCapture)
		{
			return;
		}
		FrameCapture fc = std::move(*m_frameCapture);
		m_frameCapture.reset();

		bool ok = false;
		if (fc.buffer.IsValid() && fc.mapped != nullptr)
		{
			vkQueueWaitIdle(static_cast<VkQueue>(m_queue));
			const std::uint32_t byteSize = fc.width * fc.height * BytesPerPixel(fc.format);
			std::vector<std::uint8_t> raw(byteSize);
			std::memcpy(raw.data(), fc.mapped, byteSize);
			std::vector<std::uint8_t> rgba = ConvertToRgba(fc.format, raw.data(), fc.width, fc.height);
			ok = !rgba.empty() && WritePng(fc.path, rgba.data(), fc.width, fc.height);
		}
		if (fc.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(fc.buffer);
		}
		if (!ok)
		{
			AE_WARN(LogCategory::Render, "ScreenshotService: frame capture to '{}' failed.", fc.path);
		}
		fc.promise.set_value(ok ? fc.path : std::string{});
	}

	void ScreenshotService::ProcessPending(void* swapchainColorImage, gpu::Extent2D swapchainExtent, gpu::Format swapchainFormat)
	{
		(void) swapchainColorImage;
		(void) swapchainExtent;
		(void) swapchainFormat;

		CompleteFrameCapture();

		Pending pending;
		{
			const std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_pending || m_pending->image == nullptr)
			{
				return;
			}
			pending = std::move(*m_pending);
			m_pending.reset();
		}

		const bool ok = Capture(pending.image, pending.extent.width, pending.extent.height, pending.format, pending.aspect, static_cast<std::int32_t>(ToVkLayout(pending.srcLayout)), pending.path);
		pending.promise.set_value(ok ? pending.path : std::string{});
	}

	bool ScreenshotService::Capture(void* colorImage, std::uint32_t width, std::uint32_t height, gpu::Format format, gpu::ImageAspect aspect, std::int32_t vkSrcLayout, const std::string& path)
	{
		if (m_device == nullptr || m_pool == nullptr || colorImage == nullptr || width == 0 || height == 0)
		{
			return false;
		}
		const std::uint32_t bpp = BytesPerPixel(format);
		if (bpp == 0)
		{
			AE_WARN(LogCategory::Render, "ScreenshotService: format {} is not supported for capture to '{}'.", static_cast<int>(format), path);
			return false;
		}
		const VkImageAspectFlags vkAspect = (aspect == gpu::ImageAspect::Depth) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		const auto srcLayout = static_cast<VkImageLayout>(vkSrcLayout);
		auto* device = static_cast<VkDevice>(m_device);
		auto* image = static_cast<VkImage>(colorImage);
		const std::uint32_t byteSize = width * height * bpp;

		const gpu::MappedBufferDesc bufDesc{.size = byteSize, .usage = gpu::BufferUsage::TransferDst, .memoryUsage = gpu::MappedMemoryUsage::Auto, .debugName = "Screenshot"};
		const gpu::BufferHandle bufHandle = gpu::ResourceRegistry::CreateMappedBuffer(bufDesc);
		if (!bufHandle.IsValid())
		{
			return false;
		}
		auto* buffer = static_cast<VkBuffer>(gpu::ResourceRegistry::ResolveBufferVkHandle(bufHandle));
		const void* mapped = gpu::ResourceRegistry::ResolveMappedBuffer(bufHandle).mappedPtr;

		const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = static_cast<VkCommandPool>(m_pool), .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS)
		{
			gpu::ResourceRegistry::Destroy(bufHandle);
			return false;
		}
		const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		vkBeginCommandBuffer(cmd, &bi);

		const auto barrier = [&](VkImageLayout oldL, VkImageLayout newL, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
		{
			const VkImageMemoryBarrier b{
			        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			        .srcAccessMask = srcA,
			        .dstAccessMask = dstA,
			        .oldLayout = oldL,
			        .newLayout = newL,
			        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			        .image = image,
			        .subresourceRange = {vkAspect, 0, 1, 0, 1},
			};
			vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
		};

		VkAccessFlags sourceAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		switch (srcLayout)
		{
			case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
				sourceAccess = VK_ACCESS_SHADER_READ_BIT;
				break;
			case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
				sourceAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				break;
			case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
				sourceAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				sourceStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				break;
			case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
				sourceAccess = VK_ACCESS_TRANSFER_READ_BIT;
				sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
				break;
			case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
				sourceAccess = 0;
				sourceStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
				break;
			default:
				break;
		}

		barrier(srcLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceAccess, VK_ACCESS_TRANSFER_READ_BIT, sourceStage, VK_PIPELINE_STAGE_TRANSFER_BIT);

		const VkBufferImageCopy region{
		        .bufferOffset = 0,
		        .bufferRowLength = 0,
		        .bufferImageHeight = 0,
		        .imageSubresource = {vkAspect, 0, 0, 1},
		        .imageOffset = {0, 0, 0},
		        .imageExtent = {width, height, 1},
		};
		vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

		barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, srcLayout, VK_ACCESS_TRANSFER_READ_BIT, sourceAccess, VK_PIPELINE_STAGE_TRANSFER_BIT, sourceStage);

		vkEndCommandBuffer(cmd);

		const VkFenceCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence fence = VK_NULL_HANDLE;
		vkCreateFence(device, &fi, nullptr, &fence);
		const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
		bool ok = vkQueueSubmit(static_cast<VkQueue>(m_queue), 1, &si, fence) == VK_SUCCESS;
		if (ok)
		{
			ok = vkWaitForFences(device, 1, &fence, VK_TRUE, 2'000'000'000ull) == VK_SUCCESS; // 2s
		}

		if (ok && mapped != nullptr)
		{
			std::vector<std::uint8_t> raw(byteSize);
			std::memcpy(raw.data(), mapped, byteSize);
			std::vector<std::uint8_t> rgba = ConvertToRgba(format, raw.data(), width, height);
			ok = !rgba.empty() && WritePng(path, rgba.data(), width, height);
		}
		else
		{
			ok = false;
		}

		vkDestroyFence(device, fence, nullptr);
		vkFreeCommandBuffers(device, static_cast<VkCommandPool>(m_pool), 1, &cmd);
		gpu::ResourceRegistry::Destroy(bufHandle);
		if (!ok)
		{
			AE_WARN(LogCategory::Render, "ScreenshotService: capture to '{}' failed.", path);
		}
		return ok;
	}
} // namespace aether
