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
	struct WebShadowTargets
	{
		WGPUTexture color = nullptr, depth = nullptr;
		WGPUTextureView sample = nullptr;
		~WebShadowTargets();
	};

	// Owned by one submitted frame; destruction happens after queue retirement.
	struct WebMeshFrame
	{
		WGPUBuffer uniform = nullptr, instances = nullptr, skin = nullptr, lights = nullptr;
		WGPUBindGroup bindings = nullptr;
		std::vector<WGPUBindGroup> materials;
		WGPUTexture depth = nullptr;
		WGPUTextureView depthView = nullptr;
		std::shared_ptr<WebShadowTargets> shadowTargets;
		WGPUBuffer shadows = nullptr;
		std::vector<WGPUBuffer> shadowUniforms;
		std::vector<WGPUBindGroup> shadowBindings;
		~WebMeshFrame();
	};

	class WebMeshRenderer
	{
	public:
		~WebMeshRenderer();
		bool Initialize(WGPUDevice device);
		bool Upload(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices);
		[[nodiscard]] std::expected<std::vector<gpu::MeshGpuInstance>, std::string> Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures) const;
		void Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView color,
		        unsigned width,
		        unsigned height,
		        const RenderFramePacket& packet,
		        std::span<const gpu::MeshGpuInstance> instances,
		        std::span<const WGPUTexture> textures,
		        std::unique_ptr<WebMeshFrame>& frame,
		        bool hdr = false);
		void Shutdown();
		std::string error;

	private:
		struct MeshSlot
		{
			WGPUBuffer vertices = nullptr, indices = nullptr;
			std::uint32_t indexCount = 0;
			std::uint32_t jointCount = 0;
			bool skinReady = false;
		};

		std::vector<MeshSlot> m_meshes;
		std::array<WGPURenderPipeline, 18> m_pipelines{};
		WGPURenderPipeline m_shadow_pipeline = nullptr;
		WGPUBindGroupLayout m_frame_layout = nullptr, m_material_layout = nullptr;
		WGPUBindGroupLayout m_shadow_frame_layout = nullptr;
		WGPUTexture m_white = nullptr;
		WGPUSampler m_sampler = nullptr;
		std::size_t m_bytes = 0;
		unsigned m_max_texture_dimension = 0;
		std::shared_ptr<WebShadowTargets> m_shadow_targets;
		std::shared_ptr<WebShadowTargets> m_shadow_fallback;
	};
} // namespace aether::webgpu
