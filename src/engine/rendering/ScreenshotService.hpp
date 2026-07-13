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
	// be called from any thread (e.g. the control endpoint on the main thread); the
	// GPU readback itself runs on the render thread via ProcessPending(), because
	// vkQueueSubmit is externally synchronized and must happen where the frame is
	// submitted. Fully self-contained: it uses its own one-time command buffer +
	// fence and never touches the frame's command recording.
	class ScreenshotService
	{
	public:
		ScreenshotService() = default;

		void Init(gpu::Device device, std::uint32_t graphicsQueueFamily, gpu::Queue queue);
		void Shutdown();

		// Request a capture of the SWAPCHAIN (whole editor frame) to `absolutePath`
		// (a .png). Returns a future resolving to the path on success, else "".
		[[nodiscard]] std::future<std::string> Request(std::string absolutePath);

		// Request a capture of a SPECIFIC image (e.g. a registered render target or
		// asset texture) currently in `srcLayout`. 8-bit color is written directly;
		// depth (D32) is normalized to grayscale and HDR (RGBA16F) is tonemapped.
		[[nodiscard]] std::future<std::string> RequestImage(void* image, gpu::Extent2D extent, gpu::Format format, gpu::ImageAspect aspect, gpu::ImageLayout srcLayout, std::string absolutePath);

		// Render thread, called from Swapchain's pre-present hook while the acquired
		// swapchain image is still in COLOR_ATTACHMENT: if a whole-frame request is
		// pending, record the copy into the frame's own command buffer (`cmd`). The
		// readback is completed later by ProcessPending. `cmd`/`image` are opaque
		// VkCommandBuffer/VkImage. Cheap no-op when nothing is pending.
		void RecordFrameCapture(void* cmd, void* swapchainImage, gpu::Extent2D extent, gpu::Format format);

		// Render thread, once per frame AFTER present: completes an in-frame swapchain
		// capture recorded by RecordFrameCapture, then services any pending
		// specific-image (non-swapchain) request via a self-contained one-shot copy.
		void ProcessPending(void* swapchainColorImage, gpu::Extent2D swapchainExtent, gpu::Format swapchainFormat);

		[[nodiscard]] bool IsInitialized() const noexcept
		{
			return m_device != nullptr;
		}

	private:
		// vkSrcLayout is a raw VkImageLayout value (kept as int so the header stays
		// free of the Vulkan headers).
		bool Capture(void* image, std::uint32_t width, std::uint32_t height, gpu::Format format, gpu::ImageAspect aspect, std::int32_t vkSrcLayout, const std::string& path);

		gpu::Device m_device = nullptr;
		gpu::Queue m_queue = nullptr;
		gpu::CommandPool m_pool = nullptr;
		std::uint32_t m_queueFamily = 0;

		std::mutex m_mutex;

		struct Pending
		{
			std::string path;
			void* image = nullptr; // null => capture the swapchain image ProcessPending supplies
			gpu::Extent2D extent;
			gpu::Format format = gpu::Format::Undefined;
			gpu::ImageAspect aspect = gpu::ImageAspect::Color;
			gpu::ImageLayout srcLayout = gpu::ImageLayout::ShaderReadOnly;
			std::promise<std::string> promise;
		};

		std::optional<Pending> m_pending;

		// A whole-frame capture whose copy has been recorded into the frame's command
		// buffer (by RecordFrameCapture) and is awaiting readback in ProcessPending.
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
