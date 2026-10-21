#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>
#include <webgpu/webgpu.h>

#include "mesh/MeshVertex.hpp"
#include "gpu/MeshPass.hpp"

namespace aether
{
	struct RenderFramePacket;
}

namespace aether::webgpu
{
	// Owned by one submitted frame; destruction happens after queue retirement.
	struct WebMeshFrame
	{
		WGPUBuffer uniform = nullptr, instances = nullptr;
		WGPUBindGroup bindings = nullptr;
		WGPUTexture depth = nullptr;
		WGPUTextureView depthView = nullptr;
		~WebMeshFrame();
	};

	class WebMeshRenderer
	{
	public:
		~WebMeshRenderer();
		bool Initialize(WGPUDevice device);
		bool Upload(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] std::expected<std::vector<gpu::MeshGpuInstance>, std::string> Prepare(const RenderFramePacket& packet) const;
		void Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView color,
		        unsigned width,
		        unsigned height,
		        const RenderFramePacket& packet,
		        std::span<const gpu::MeshGpuInstance> instances,
		        std::unique_ptr<WebMeshFrame>& frame);
		void Shutdown();
		std::string error;

	private:
		struct MeshSlot
		{
			WGPUBuffer vertices = nullptr, indices = nullptr;
			std::uint32_t indexCount = 0;
		};

		std::vector<MeshSlot> m_meshes;
		WGPURenderPipeline m_pipeline = nullptr;
		std::size_t m_bytes = 0;
	};
} // namespace aether::webgpu
