#include "ui/UiRenderer.hpp"

#include <cstring>
#include <string>
#include <string_view>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/OneShotCmd.hpp"
#include "gpu/PushConstantsBytes.hpp"
#include "gpu/UploadContext.hpp"
#include "io/FileSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderGraph.hpp"
#include "scene/World.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiDrawBuilder.hpp"
#include "ui/UiLayoutSystem.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace aether::ui
{
	namespace
	{
		// Push-constant layout - MUST match ShapesPush in shaders/ui_shapes.slang
		// byte-for-byte: float4 screenSize (16B) + DevicePtr<DrawCommandData>
		// (8B - a bare device address under the hood) + 2x uint32 padding (8B).
		struct ShapesPush
		{
			glm::vec4 screenSize; // .xy = viewport dimensions
			std::uint64_t commandData;
			std::uint32_t pad0;
			std::uint32_t pad1;
		};

		static_assert(sizeof(ShapesPush) == 32, "ShapesPush must match shaders/ui_shapes.slang ShapesPush layout");

		constexpr std::uint32_t kInitialCommandCapacity = 256;
		constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;
	} // namespace

	void UiRenderer::Init(GpuDevice& gpu, gpu::UploadContext& upload, TextureRegistry& textures, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();

		m_gpu = &gpu;
		m_upload = &upload;
		m_textures = &textures;

		// ui_shapes.slang samples the bindless resource heap (g_textures[] /
		// g_linearSampler at set 0, for textured rects + SDF glyphs), so the
		// pipeline needs the descriptor-heap mapping info chained into the
		// VkShaderCreateInfoEXT pNext, exactly like GTAOPass/PostProcessStack/
		// TexturePreview do for their bindless-sampling pipelines.
		const gpu::GraphicsPipelineDesc desc{
		        .shaderVfsPath = "shaders://ui_shapes.spv",
		        .fragmentVfsPath = nullptr, // shares the vertex module (single ui_shapes.spv has both stages)
		        .vertexEntry = "vertexMain",
		        .fragmentEntry = "fragmentMain",
		        .colorFormat = colorFormat,
		        .depthFormat = gpu::Format::Undefined,
		        .depthTestEnable = false,
		        .depthWriteEnable = false,
		        .blendEnable = true,
		        .topology = gpu::PrimitiveTopology::TriangleList,
		        .polygonMode = gpu::PolygonMode::Fill,
		        .cullMode = gpu::CullMode::None,
		        .debugName = "UI.Shapes",
		        .descriptorHeapMappings = gpu.GetBindlessManager().GetDescriptorHeapMappings(),
		};

		m_pipeline = gpu::ResourceRegistry::CreateGraphicsPipeline(gpu.GetDevice(), desc);
		if (!m_pipeline.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to create ui_shapes pipeline");
		}

		m_defaultFontReady = EnsureFontAtlasUploaded("Roboto");
	}

	void UiRenderer::Shutdown()
	{
		AE_PROFILE_ZONE();

		for (Frame& frame: m_frames)
		{
			if (frame.buffer.IsValid())
			{
				gpu::ResourceRegistry::Destroy(frame.buffer);
			}
			frame = Frame{};
		}

		if (m_pipeline.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_pipeline);
			m_pipeline = {};
		}

		if (m_defaultFontAtlas.IsValid())
		{
			gpu::ResourceRegistry::Destroy(m_defaultFontAtlas);
			m_defaultFontAtlas = {};
		}

		m_defaultFontReady = false;
		m_textures = nullptr;
		m_upload = nullptr;
		m_gpu = nullptr;
	}

	bool UiRenderer::EnsureFontAtlasUploaded(std::string_view name)
	{
		FontAsset* font = nullptr;
		if (m_fontRegistry.Load(name) != nullptr)
		{
			font = m_fontRegistry.GetMutable(name);
		}
		if (font == nullptr)
		{
			return false;
		}
		if (font->atlasBindlessSlot != kInvalidBindlessSlot)
		{
			return true;
		}
		if (m_gpu == nullptr || m_upload == nullptr || !m_upload->IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: cannot upload font atlas '{}', GPU upload context is unavailable", name);
			return false;
		}

		const std::string atlasPath = "engine://fonts/" + std::string(name) + "-Regular.fontatlas";
		const auto atlasBytes = io::FileSystem::ReadFile(atlasPath);
		if (!atlasBytes.has_value())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to read '{}'", atlasPath);
			return false;
		}
		if (atlasBytes->size() < sizeof(FontAtlasHeader))
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: '{}' is too small for a FontAtlasHeader", atlasPath);
			return false;
		}

		FontAtlasHeader header{};
		std::memcpy(&header, atlasBytes->data(), sizeof(FontAtlasHeader));
		if (header.magic != kFontAtlasMagic || header.width == 0 || header.height == 0)
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: '{}' has an invalid font atlas header", atlasPath);
			return false;
		}

		const std::size_t texelBytes = static_cast<std::size_t>(header.width) * static_cast<std::size_t>(header.height);
		const std::size_t expectedSize = sizeof(FontAtlasHeader) + texelBytes;
		if (atlasBytes->size() != expectedSize)
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: '{}' size {} does not match atlas dimensions (expected {})", atlasPath, atlasBytes->size(), expectedSize);
			return false;
		}

		const std::string debugName = "UI.FontAtlas." + std::string(name);
		const gpu::TextureDesc desc{
		        .format = gpu::Format::R8Unorm,
		        .extent = {header.width, header.height},
		        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | gpu::ImageUsage::HostTransfer,
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = debugName.c_str(),
		};
		gpu::TextureHandle atlas = gpu::ResourceRegistry::CreateTexture(desc);
		if (!atlas.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to create font atlas texture '{}'", atlasPath);
			return false;
		}

		const gpu::Image image = gpu::ResourceRegistry::ResolveTextureImage(atlas);
		const void* texels = atlasBytes->data() + sizeof(FontAtlasHeader);
		const std::int32_t copyResult = vkutil::HostCopyToImage(m_gpu->GetDevice(), image, texels, header.width, header.height);
		if (copyResult != 0)
		{
			gpu::ResourceRegistry::Destroy(atlas);
			AE_ERROR(LogCategory::UI, "UiRenderer: HostCopyToImage failed for '{}' (VkResult={})", atlasPath, copyResult);
			return false;
		}

		gpu::OneShotCmd cmd;
		if (!cmd.Begin(m_gpu->GetDevice(), m_upload->GetCommandPool()))
		{
			gpu::ResourceRegistry::Destroy(atlas);
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to begin font atlas upload barrier command");
			return false;
		}
		cmd.CmdList().ImageMemoryBarrier(
		        image, gpu::ImageLayout::General, gpu::ImageLayout::ShaderReadOnly, gpu::ImageAspect::Color, gpu::PipelineStage::AllCommands, gpu::AccessFlags::None, gpu::PipelineStage::FragmentShader, gpu::AccessFlags::ShaderRead);
		if (!cmd.EndAndSubmit(m_gpu->GetGraphicsQueue()))
		{
			gpu::ResourceRegistry::Destroy(atlas);
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to submit font atlas upload barrier command");
			return false;
		}

		gpu::ResourceRegistry::EnsureBindlessSampled(atlas, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
		const std::uint32_t slot = gpu::ResourceRegistry::GetBindlessSampledSlot(atlas);
		if (slot == kInvalidBindlessSlot)
		{
			gpu::ResourceRegistry::Destroy(atlas);
			AE_ERROR(LogCategory::UI, "UiRenderer: bindless registration failed for '{}'", atlasPath);
			return false;
		}

		font->atlasBindlessSlot = slot;
		m_defaultFontAtlas = atlas;
		AE_INFO(LogCategory::UI, "UiRenderer: uploaded font atlas '{}' ({}x{}, slot {})", atlasPath, header.width, header.height, slot);
		return true;
	}

	void UiRenderer::EnsureCapacity(Frame& frame, std::uint32_t count)
	{
		if (frame.capacity >= count && frame.buffer.IsValid())
		{
			return;
		}

		std::uint32_t newCapacity = frame.capacity == 0 ? kInitialCommandCapacity : frame.capacity;
		while (newCapacity < count)
		{
			newCapacity *= 2;
		}

		if (frame.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(frame.buffer);
			frame.buffer = {};
			frame.mapped = nullptr;
			frame.address = 0;
		}

		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(newCapacity) * sizeof(UiDrawCommand),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "UI.Commands",
		};
		frame.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!frame.buffer.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to allocate command buffer ({} commands)", newCapacity);
			frame.capacity = 0;
			return;
		}

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(frame.buffer);
		frame.mapped = view.mappedPtr;
		frame.address = view.deviceAddress;
		frame.capacity = newCapacity;
	}

	void UiRenderer::BuildFrame(glm::vec2 outputExtent, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();

		Frame& frame = m_frames[frameSlot % kFrames];
		frame.count = 0;
		// Set the extent on the slot unconditionally (even with zero commands),
		// so it always matches whatever this slot's buffer holds when the render
		// thread later executes the pass.
		frame.extent = outputExtent;

		if (m_world == nullptr)
		{
			return;
		}

		ResolveCanvases(*m_world, outputExtent);
		BuildDrawCommands(*m_world, m_scratch, m_defaultFontReady ? &m_fontRegistry : nullptr, m_textures);

		const auto count = static_cast<std::uint32_t>(m_scratch.size());
		if (count == 0)
		{
			return;
		}

		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			return;
		}

		const auto byteSize = static_cast<gpu::DeviceSize>(count) * sizeof(UiDrawCommand);
		std::memcpy(frame.mapped, m_scratch.data(), byteSize);
		gpu::ResourceRegistry::FlushMappedBuffer(frame.buffer, 0, byteSize);
		frame.count = count;
	}

	void UiRenderer::RegisterPass(RenderGraph& graph, RGImage color, gpu::Extent2D extent, BindlessManager& bindless)
	{
		if (!color.IsValid())
		{
			color = graph.GetSwapchainColor();
		}

		// This pass is registered ONCE (render-graph build time, e.g. from
		// RenderingSubsystem::RegisterPasses), not re-added every frame - matching
		// every other pass in this codebase (ShadowService, PhysicsDebugRenderer,
		// PostProcessStack, ...). It deliberately captures NO frame slot: a slot
		// baked into the Execute closure here would be a single value frozen at
		// registration, going stale the moment the real in-flight slot rotates
		// (kMaxFramesInFlight == 3). Instead the Execute below reads the LIVE slot
		// from PassContext, exactly like ShadowService's Execute lambdas use
		// ctx.frameSlot - that is what makes this draw the SAME slot BuildFrame
		// just uploaded, every frame.
		auto pass = graph.AddPass("$UiOverlay");
		if (extent.width != 0 && extent.height != 0)
		{
			pass.SetExtent(extent);
		}

		pass.WriteColor(color, gpu::LoadOp::Load, gpu::StoreOp::Store)
		        .Execute(
		                [this, &bindless](PassContext& ctx)
		                {
			                const Frame& frame = m_frames[ctx.frameSlot % kFrames];
			                if (frame.count == 0 || !m_pipeline.IsValid())
			                {
				                return;
			                }

			                gpu::CommandList& cmd = ctx.recorder;

			                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(m_pipeline);
			                cmd.BindPipeline(const_cast<void*>(resolved.state));
			                bindless.CmdBindHeaps(cmd);

			                const ShapesPush push{
			                        .screenSize = {frame.extent.x, frame.extent.y, 0.f, 0.f},
			                        .commandData = frame.address,
			                        .pad0 = 0,
			                        .pad1 = 0,
			                };
			                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                cmd.Draw(6, frame.count, 0, 0);
		                });
	}
} // namespace aether::ui
