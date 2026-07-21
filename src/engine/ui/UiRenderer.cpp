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
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "ui/FontRegistry.hpp"
#include "ui/UiComponents.hpp"
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
		struct ShapesPush
		{
			glm::vec4 screenSize;
			std::uint64_t commandData;
			std::uint32_t pad0;
			std::uint32_t pad1;
		};

		static_assert(sizeof(ShapesPush) == 32, "ShapesPush must match shaders/ui_shapes.slang ShapesPush layout");

		// Push-constant layout for UIEffect shaders - MUST match the InkPush in shaders/ui_ink.slang
		// (and any other effect shader that opts into this generic layout).
		struct EffectPush
		{
			glm::vec4 screenSize; // xy = viewport
			glm::vec4 rect;       // x, y, w, h in px
			glm::vec4 params;     // shader-defined
			glm::vec4 color0;
			glm::vec4 color1;
		};

		static_assert(sizeof(EffectPush) == 80, "EffectPush must match the effect-shader push layout");

		constexpr std::uint32_t kInitialCommandCapacity = 256;
		constexpr std::uint32_t kInvalidBindlessSlot = 0xFFFFFFFFu;
	} // namespace

	void UiRenderer::Init(GpuDevice& gpu, gpu::UploadContext& upload, TextureRegistry& textures, gpu::Format colorFormat)
	{
		AE_PROFILE_ZONE();

		m_gpu = &gpu;
		m_upload = &upload;
		m_textures = &textures;
		m_colorFormat = colorFormat;

		const gpu::GraphicsPipelineDesc desc{
		        .shaderVfsPath = "shaders://ui_shapes.spv",
		        .fragmentVfsPath = nullptr,
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

		for (auto& [name, pipe]: m_effectPipelines)
		{
			if (pipe.IsValid())
			{
				gpu::ResourceRegistry::Destroy(pipe);
			}
		}
		m_effectPipelines.clear();
		for (RetiringPipeline& retiring: m_effectPipelinesRetiring)
		{
			if (retiring.pipeline.IsValid())
			{
				gpu::ResourceRegistry::Destroy(retiring.pipeline);
			}
		}
		m_effectPipelinesRetiring.clear();

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

		// The registry recorded the paired atlas path when the meta loaded -
		// no name-based guessing here.
		const std::string& atlasPath = font->atlasPath;
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
		const gpu::TextureHandle atlas = gpu::ResourceRegistry::CreateTexture(desc);
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

	gpu::PipelineHandle UiRenderer::EffectPipeline(const std::string& shader)
	{
		if (const auto it = m_effectPipelines.find(shader); it != m_effectPipelines.end())
		{
			return it->second;
		}

		gpu::PipelineHandle handle{};
		if (m_gpu != nullptr && !shader.empty())
		{
			const std::string path = "shaders://" + shader + ".spv";
			const gpu::GraphicsPipelineDesc desc{
			        .shaderVfsPath = path.c_str(),
			        .fragmentVfsPath = nullptr,
			        .vertexEntry = "vertexMain",
			        .fragmentEntry = "fragmentMain",
			        .colorFormat = m_colorFormat,
			        .depthFormat = gpu::Format::Undefined,
			        .depthTestEnable = false,
			        .depthWriteEnable = false,
			        .blendEnable = true,
			        .topology = gpu::PrimitiveTopology::TriangleList,
			        .polygonMode = gpu::PolygonMode::Fill,
			        .cullMode = gpu::CullMode::None,
			        .debugName = "UI.Effect",
			        .descriptorHeapMappings = m_gpu->GetBindlessManager().GetDescriptorHeapMappings(),
			};
			handle = gpu::ResourceRegistry::CreateGraphicsPipeline(m_gpu->GetDevice(), desc);
			if (!handle.IsValid())
			{
				AE_ERROR(LogCategory::UI, "UiRenderer: failed to create effect pipeline for '{}'", shader);
			}
		}
		// Cache even an invalid handle so a missing shader is not retried (and re-logged) every frame.
		m_effectPipelines.emplace(shader, handle);
		return handle;
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
		frame.effects.clear();
		// thread later executes the pass.
		frame.extent = outputExtent;

		// When the shader overlay changes (a project shader recompiled), retire cached effect
		// pipelines so they rebuild from the fresh .spv; destroy the old ones a few frames later
		// once no in-flight command buffer can still reference them.
		if (const std::uint64_t gen = io::FileSystem::ShaderOverlayGeneration(); gen != m_shaderGen)
		{
			m_shaderGen = gen;
			for (auto& [name, pipe]: m_effectPipelines)
			{
				if (pipe.IsValid())
				{
					m_effectPipelinesRetiring.push_back({pipe, static_cast<int>(kFrames) + 1});
				}
			}
			m_effectPipelines.clear();
		}
		for (std::size_t i = 0; i < m_effectPipelinesRetiring.size();)
		{
			if (--m_effectPipelinesRetiring[i].framesLeft <= 0)
			{
				gpu::ResourceRegistry::Destroy(m_effectPipelinesRetiring[i].pipeline);
				m_effectPipelinesRetiring[i] = m_effectPipelinesRetiring.back();
				m_effectPipelinesRetiring.pop_back();
			}
			else
			{
				++i;
			}
		}

		if (m_world == nullptr)
		{
			return;
		}

		ResolveCanvases(*m_world, outputExtent);

		// Custom-shader UI elements (UIEffect) are drawn on top of the batched shapes, each with
		// its own pipeline. Collected here from the resolved layout; skipped when in a disabled
		// (hidden) subtree so they follow screen visibility like everything else.
		for (auto&& [ent, effect, rect]: m_world->View<UIEffect, UIRect>().each())
		{
			const Entity e = World::FromEntt(ent);
			if (ecs::HasDisabledAncestor(*m_world, e))
			{
				continue;
			}
			const gpu::PipelineHandle pipe = EffectPipeline(effect.shader);
			if (!pipe.IsValid())
			{
				continue;
			}
			frame.effects.push_back({pipe, rect.resolvedRect, effect.params, effect.color0, effect.color1, effect.background});
		}

		// Lazily upload the atlas of every font the scene's text references -
		// project fonts appear here the first frame a UIText names them.
		for (const auto& [enttEntity, text]: m_world->View<UIText>().each())
		{
			if (!text.fontName.empty() && !m_fontsTried.contains(text.fontName))
			{
				m_fontsTried.insert(text.fontName);
				EnsureFontAtlasUploaded(text.fontName);
			}
		}

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
			color = aether::RenderGraph::GetSwapchainColor();
		}

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
			                if (frame.count == 0 && frame.effects.empty())
			                {
				                return;
			                }

			                gpu::CommandList& cmd = ctx.recorder;

			                const auto drawEffect = [&](const EffectDraw& fx)
			                {
				                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(fx.pipeline);
				                cmd.BindPipeline(resolved.state);
				                bindless.CmdBindHeaps(cmd);

				                const EffectPush push{
				                        .screenSize = {frame.extent.x, frame.extent.y, 0.f, 0.f},
				                        .rect = fx.rect,
				                        .params = fx.params,
				                        .color0 = fx.color0,
				                        .color1 = fx.color1,
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(6, 1, 0, 0);
			                };

			                // Background effects (menu backdrops) draw behind the batched UI.
			                for (const EffectDraw& fx: frame.effects)
			                {
				                if (fx.background)
				                {
					                drawEffect(fx);
				                }
			                }

			                // Batched UI shapes (one instanced draw of the whole command list).
			                if (frame.count != 0 && m_pipeline.IsValid())
			                {
				                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(m_pipeline);
				                cmd.BindPipeline(resolved.state);
				                bindless.CmdBindHeaps(cmd);

				                const ShapesPush push{
				                        .screenSize = {frame.extent.x, frame.extent.y, 0.f, 0.f},
				                        .commandData = frame.address,
				                        .pad0 = 0,
				                        .pad1 = 0,
				                };
				                cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
				                cmd.Draw(6, frame.count, 0, 0);
			                }

			                // Overlay effects (transitions) draw on top of the batched UI.
			                for (const EffectDraw& fx: frame.effects)
			                {
				                if (!fx.background)
				                {
					                drawEffect(fx);
				                }
			                }
		                });
	}
} // namespace aether::ui
