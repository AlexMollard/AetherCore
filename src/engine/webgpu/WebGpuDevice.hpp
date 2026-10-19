#pragma once

#include <memory>
#include <span>
#include <cstdint>
#include "gpu/AsyncRenderLifecycle.hpp"

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	// Browser owner-thread backend. GPU completion callbacks own transient resources.
	class WebGpuDevice
	{
	public:
		WebGpuDevice();
		~WebGpuDevice();
		void Initialize();
		[[nodiscard]] gpu::RenderState State() const;
		[[nodiscard]] bool Draw(const RenderFramePacket& packet, bool capture = false);
		[[nodiscard]] bool UploadTexture(std::uint32_t slot, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);
		void Resize(std::uint32_t width, std::uint32_t height);
		void Stop();
		void LoseDeviceForTest();

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
} // namespace aether::webgpu
