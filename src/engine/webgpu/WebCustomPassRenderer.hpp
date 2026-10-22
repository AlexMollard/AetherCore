#pragma once

#include <array>
#include <functional>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu.h>

#include "rendering/RenderFramePacket.hpp"

// Thread/ownership contract: every method runs on the single browser main thread that owns the
// WGPUDevice (WebGpuDevice::Draw and its upload entry points). Prepare reads only the
// RenderFramePacket and touches no GPU object, so invalid or over-budget submissions fail with an
// explicit diagnostic before any GPU work is submitted. Encode appends its uniform/data buffers and
// bind groups to the caller-owned WebCustomPassFrame, which the per-frame owner keeps alive until
// the queue reports the submission complete; the frame destructor then releases them. Shader
// pipelines are immutable per stem and live until Shutdown.
namespace aether::webgpu
{
	struct WebCustomPassFrame
	{
		std::vector<WGPUBuffer> buffers;
		std::vector<WGPUBindGroup> bindings;
		~WebCustomPassFrame();
	};

	class WebCustomPassRenderer
	{
	public:
		static constexpr std::size_t kMaxShaders = 64;
		static constexpr std::size_t kMaxSubmissions = 64;
		static constexpr std::size_t kMaxVec4PerSubmission = 4096;
		static constexpr std::size_t kMaxVec4PerFrame = 65536;
		static constexpr std::size_t kMaxShaderBytes = 1024 * 1024;

		~WebCustomPassRenderer();
		bool Initialize(WGPUDevice device);
		bool UploadShader(WGPUDevice device, std::string_view stem, std::string_view wgsl);
		[[nodiscard]] bool HasShader(std::string_view stem) const;
		[[nodiscard]] std::expected<void, std::string> Prepare(const RenderFramePacket& packet) const;
		void Encode(WGPUDevice device,
		        WGPUQueue queue,
		        WGPUCommandEncoder encoder,
		        WGPUTextureView color,
		        unsigned width,
		        unsigned height,
		        const RenderFramePacket& packet,
		        CustomPassStage stage,
		        bool hdrTarget,
		        bool clearFirst,
		        std::unique_ptr<WebCustomPassFrame>& frame);
		void Shutdown();
		std::string error;

	private:
		struct StemHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view value) const noexcept
			{
				return std::hash<std::string_view>{}(value);
			}
		};

		// [0] = RGBA16Float scene HDR target, [1] = RGBA8Unorm display target.
		std::unordered_map<std::string, std::array<WGPURenderPipeline, 2>, StemHash, std::equal_to<>> m_pipelines;
		WGPUBindGroupLayout m_layout = nullptr;
		WGPUPipelineLayout m_pipeline_layout = nullptr;
	};
} // namespace aether::webgpu
