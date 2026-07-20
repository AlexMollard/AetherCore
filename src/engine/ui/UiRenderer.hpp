#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "gpu/ResourceRegistry.hpp"
#include "rendering/RenderGraphTypes.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiDrawCommand.hpp"

namespace aether
{
	class TextureRegistry;
	class World;
	class RenderGraph;
	class BindlessManager;
	class GpuDevice;
	struct PassContext;

	namespace gpu
	{
		class UploadContext;
	}
} // namespace aether

namespace aether::ui
{
	//      layout against m_world, builds the command list, uploads it into the
	class UiRenderer
	{
	public:
		void Init(GpuDevice& gpu, gpu::UploadContext& upload, TextureRegistry& textures, gpu::Format colorFormat);
		void Shutdown();

		void SetWorld(World* world)
		{
			m_world = world;
		}

		// Resolves layout + builds draw commands for m_world, uploads into frame
		void BuildFrame(glm::vec2 outputExtent, std::uint32_t frameSlot);

		void RegisterPass(RenderGraph& graph, RGImage color, gpu::Extent2D extent, BindlessManager& bindless);

	private:
		static constexpr std::uint32_t kFrames = aether::kMaxFramesInFlight;

		struct Frame
		{
			gpu::BufferHandle buffer{};
			void* mapped = nullptr;
			std::uint64_t address = 0;
			std::uint32_t capacity = 0;
			std::uint32_t count = 0;
			// (not a shared member) so a producer-thread BuildFrame for the next
			glm::vec2 extent{0.f};
		};

		void EnsureCapacity(Frame& frame, std::uint32_t count);
		bool EnsureFontAtlasUploaded(std::string_view name);

		World* m_world = nullptr;
		GpuDevice* m_gpu = nullptr;
		gpu::UploadContext* m_upload = nullptr;
		const TextureRegistry* m_textures = nullptr;
		FontRegistry m_fontRegistry;
		gpu::TextureHandle m_defaultFontAtlas{};
		bool m_defaultFontReady = false;
		// Font names whose atlas upload was attempted (success or not) - one try each.
		std::unordered_set<std::string> m_fontsTried;
		gpu::PipelineHandle m_pipeline{};
		std::array<Frame, kFrames> m_frames{};
		std::vector<UiDrawCommand> m_scratch;
	};
} // namespace aether::ui
