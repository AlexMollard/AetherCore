#pragma once

#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>

#include "gpu/GpuEnums.hpp"
#include "gpu/GpuFormat.hpp"
#include "gpu/GpuHandles.hpp"
#include "gpu/GpuTypes.hpp"

namespace aether
{
	// Captures the just-presented swapchain image to a .png on disk. Request() may
	class ScreenshotService
	{
	public:
		ScreenshotService() = default;

		void Init(gpu::Device device, std::uint32_t graphicsQueueFamily, gpu::Queue queue);
		void Shutdown();

		[[nodiscard]] std::future<std::string> Request(std::string absolutePath);

		[[nodiscard]] std::future<std::string> RequestImage(void* image, gpu::Extent2D extent, gpu::Format format, gpu::ImageAspect aspect, gpu::ImageLayout srcLayout, std::string absolutePath);

		// Render thread, called from Swapchain's pre-present hook while the acquired
		void RecordFrameCapture(void* cmd, void* imageV, gpu::Extent2D extent, gpu::Format format);

		// Render thread, once per frame AFTER present: completes an in-frame swapchain
		void ProcessPending(void* swapchainColorImage, gpu::Extent2D swapchainExtent, gpu::Format swapchainFormat);

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_device != nullptr;
		}

	private:
		bool Capture(void* image, std::uint32_t width, std::uint32_t height, gpu::Format format, gpu::ImageAspect aspect, std::int32_t vkSrcLayout, const std::string& path);

		gpu::Device m_device = nullptr;
		gpu::Queue m_queue = nullptr;
		gpu::CommandPool m_pool = nullptr;
		std::uint32_t m_queueFamily = 0;

		std::mutex m_mutex;

		struct Pending
		{
			std::string path;
			void* image = nullptr;
			gpu::Extent2D extent;
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::ImageLayout srcLayout = gpu::ImageLayout::ShaderReadOnly;
			std::promise<std::string> promise;
		};

		std::optional<Pending> m_pending;

		struct FrameCapture
		{
			std::string path;
			std::promise<std::string> promise;
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			gpu::Format format = gpu::Format::Undefined;
		};

		std::optional<FrameCapture> m_frameCapture;

		void CompleteFrameCapture();
	};
} // namespace aether
