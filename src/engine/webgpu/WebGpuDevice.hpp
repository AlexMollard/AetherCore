#pragma once

#include <memory>
#include <span>
#include <cstdint>
#include <string>
#include "gpu/AsyncRenderLifecycle.hpp"
#include "mesh/MeshVertex.hpp"

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
		WebGpuDevice(const WebGpuDevice&) = delete;
		WebGpuDevice& operator=(const WebGpuDevice&) = delete;
		void Initialize();
		[[nodiscard]] gpu::RenderState State() const;
		[[nodiscard]] bool Draw(const RenderFramePacket& packet, bool capture = false);
		[[nodiscard]] bool UploadTexture(std::uint32_t slot, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba, bool srgb = false, bool mipmapped = false);
		[[nodiscard]] bool UploadMesh(std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] const std::string& MeshUploadError() const;
		void Resize(std::uint32_t width, std::uint32_t height);
		void Stop();
#if AE_WEB_TEST_BRIDGE
		void LoseDeviceForTest();
#endif

	private:
		struct Impl;
		std::shared_ptr<Impl> m_impl;
	};
} // namespace aether::webgpu
