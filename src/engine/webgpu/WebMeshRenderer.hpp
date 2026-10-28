#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
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
	struct WebMeshInputs
	{
		WGPUTextureView sceneDepth = nullptr;
		WGPUTextureView normalRoughness = nullptr;
		WGPUTextureView albedo = nullptr;
		WGPUTextureView ambientOcclusion = nullptr;
	};

	struct WebOpaqueInputs : WebMeshInputs
	{
		WGPUTextureView depthAttachment = nullptr;
	};

	struct WebMeshPreparedData;
	struct WebShadowInputs
	{
		WGPUTextureView atlas = nullptr;
		std::array<glm::mat4, 3> viewProj{};
		std::array<glm::vec4, 3> regions{}; // Pixel x/y/width/height in the 4096 x 7168 atlas.
		glm::vec4 depthBias{}, normalOffset{}, penumbraScale{}, splits{};
		bool enabled = false;
	};
	struct WebShadowTargets
	{
		WGPUTexture color = nullptr, depth = nullptr;
		WGPUTextureView sample = nullptr;
		WGPUTextureView sceneSample = nullptr;
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
		WGPUBuffer shadows = nullptr, effects = nullptr;
		std::vector<WGPUBuffer> shadowUniforms;
		std::vector<WGPUBindGroup> shadowBindings;
		std::unique_ptr<WebMeshPreparedData> prepared;
		WGPUBindGroup auxiliaryBindings = nullptr, inputBindings = nullptr;
		WGPUTextureView opaqueDepthAttachment = nullptr;
		WebShadowInputs directional;
		std::uint32_t cameraVisibleCount = 0, frustumCulledCount = 0, distanceCulledCount = 0;
		~WebMeshFrame();
	};

	class WebMeshRenderer
	{
	public:
		~WebMeshRenderer();
		bool Initialize(WGPUDevice device);
		bool UploadShader(WGPUDevice device, std::uint32_t slot, std::string_view wgsl);
		bool Upload(WGPUDevice device, WGPUQueue queue, std::uint32_t slot, std::span<const MeshVertex> vertices, std::span<const std::uint32_t> indices);
		void Release(std::uint32_t slot);
		[[nodiscard]] bool HasUploadCapacity(std::size_t bytes) const { return bytes <= 64 * 1024 * 1024 - m_bytes; }
		[[nodiscard]] std::size_t AvailableUploadBytes() const { return 64 * 1024 * 1024 - m_bytes; }
		[[nodiscard]] std::expected<std::vector<gpu::MeshGpuInstance>, std::string> Prepare(const RenderFramePacket& packet, std::span<const WGPUTexture> textures) const;
		void EncodeAuxiliary(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        unsigned width,
		        unsigned height,
		        const RenderFramePacket& packet,
		        std::span<const gpu::MeshGpuInstance> instances,
		        std::span<const WGPUTexture> textures,
		        const WebOpaqueInputs& inputs,
		        std::unique_ptr<WebMeshFrame>& frame);
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
		        bool hdr = false,
		        const WebMeshInputs& inputs = {});
		void Shutdown();
		std::string error;

	private:
		void PrepareFrame(WGPUDevice device, WGPUQueue queue, unsigned width, unsigned height,
		        const RenderFramePacket& packet, std::span<const gpu::MeshGpuInstance> instances,
		        std::span<const WGPUTexture> textures, std::unique_ptr<WebMeshFrame>& frame, bool hdr);
		struct MeshSlot
		{
			WGPUBuffer vertices = nullptr, indices = nullptr;
			std::uint32_t indexCount = 0;
			std::uint32_t jointCount = 0;
			bool skinReady = false;
			glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};
			std::size_t bytes = 0;
		};

		std::vector<MeshSlot> m_meshes;
		std::array<WGPURenderPipeline, 18> m_pipelines{};
		std::vector<std::array<WGPURenderPipeline, 18>> m_custom_pipelines;
		std::vector<std::array<WGPURenderPipeline, 3>> m_custom_auxiliary;
		WGPURenderPipeline m_shadow_pipeline = nullptr;
		WGPURenderPipeline m_auxiliary_pipeline = nullptr;
		WGPUBindGroupLayout m_input_layout = nullptr;
		WGPUBindGroupLayout m_frame_layout = nullptr, m_material_layout = nullptr;
		WGPUBindGroupLayout m_shadow_frame_layout = nullptr;
		WGPUTexture m_white = nullptr;
		WGPUSampler m_sampler = nullptr, m_scene_sampler = nullptr;
		std::size_t m_bytes = 0;
		unsigned m_max_texture_dimension = 0;
		std::shared_ptr<WebShadowTargets> m_shadow_targets;
		std::shared_ptr<WebShadowTargets> m_shadow_fallback;
	};
} // namespace aether::webgpu
