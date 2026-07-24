#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string_view>

#include <glm/glm.hpp>

#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/RenderGraphTypes.hpp"

namespace aether
{
	class BindlessManager;
	class FrameConstantsBuffer;
	class GpuDevice;
	class RenderGraph;

	// Packed 2D light record uploaded to the GPU. Mirrors GpuLight2D in shaders/light2d.slang.
	struct GpuLight2D
	{
		glm::vec4 posRadiusKind{0.0f}; // xy = world pos, z = radius, w = kind (0 point, 1 spot)
		glm::vec4 colorIntensity{0.0f}; // rgb = colour, a = intensity
		glm::vec4 spotDirCone{0.0f};    // xy = aim dir, z = cosInner, w = cosOuter
	};

	// Screen-space 2D light map. After the sprite/tile layer is drawn into the scene HDR colour, this
	// fullscreen MULTIPLY pass modulates it by (ambient + every point/spot light's contribution), so the
	// dark cave recedes to ambient and lights carve bright/tinted pools out of the pixel art. Solid tiles
	// are rasterised into an occluder mask that the composite ray-marches, so walls cast shadows. Consumes
	// the same engine PointLight/SpotLight the 3D renderer uses; engages only on the 2D presentation path.
	class Light2DCompositor
	{
	public:
		void Initialize(GpuDevice& gpu, gpu::Format colorFormat);
		void Shutdown();

		// Snapshots the frame's lights and shadow occluders (from the packet) into mapped buffers.
		// No-ops (records zero lights, so RegisterPass self-skips) for non-2D scenes or scenes with no
		// lights, leaving those frames unmodified.
		void BeginFrame(const RenderFramePacket& packet, std::uint32_t frameSlot);
		void EndFrame();

		// Registers the occluder-mask pass and the light composite pass. `color` is the scene HDR colour
		// the composite multiplies; the mask is a graph transient sized to `extent`.
		void RegisterPass(RenderGraph& graph,
		        RGImage color,
		        gpu::Extent2D extent,
		        BindlessManager& bindless,
		        std::string_view name = "$Light2DComposite",
		        const FrameConstantsBuffer* frameConstants = nullptr,
		        const std::atomic<bool>* enabled = nullptr);

	private:
		static constexpr std::uint32_t kFrames = kMaxFramesInFlight;

		struct GpuBuffer
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			gpu::DeviceAddress address = 0;
			std::uint32_t capacity = 0;
			std::uint32_t count = 0;
		};

		struct Frame
		{
			GpuBuffer lights;
			GpuBuffer occluders;
			glm::vec4 ambient{0.0f}; // rgb ambient floor, a = clamp ceiling
		};

		void EnsureCapacity(GpuBuffer& buffer, std::uint32_t count, std::size_t stride, const char* debugName);

		std::array<Frame, kFrames> m_frames{};
		gpu::PipelineHandle m_pipeline{};         // light2d composite (multiply)
		gpu::PipelineHandle m_occluderPipeline{}; // occluder2d mask (R8)
	};
} // namespace aether
