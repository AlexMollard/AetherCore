#include "ui/UiRenderer.hpp"

#include <algorithm>
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

		// Push layout for a per-element material draw. The first 32 bytes are identical to ShapesPush
		// (the shared ui_shapes vertex shader reads only those); the trailing fx fields are read by
		// the material's custom fragment. MUST match the ShapesPush/params layout in ui_material shaders.
		struct MaterialPush
		{
			glm::vec4 screenSize;
			std::uint64_t commandData;
			std::uint32_t pad0;
			std::uint32_t pad1;
			glm::vec4 params;
			glm::vec4 color0;
			glm::vec4 color1;
		};

		static_assert(sizeof(MaterialPush) == 80, "MaterialPush must match the ui_material push layout");

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
		for (auto& [name, pipe]: m_materialPipelines)
		{
			if (pipe.IsValid())
			{
				gpu::ResourceRegistry::Destroy(pipe);
			}
		}
		m_materialPipelines.clear();
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

		// Finish on the host when supported (no queue submit / fence); fall back to the
		// one-shot barrier on devices without SHADER_READ_ONLY in the host-copy layouts.
		if (vkutil::SupportsHostImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
		{
			if (vkutil::HostTransitionImageToShaderRead(m_gpu->GetDevice(), image) != 0)
			{
				gpu::ResourceRegistry::Destroy(atlas);
				AE_ERROR(LogCategory::UI, "UiRenderer: host layout transition failed for '{}'", atlasPath);
				return false;
			}
		}
		else
		{
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

	gpu::PipelineHandle UiRenderer::MaterialPipeline(const std::string& shader)
	{
		if (const auto it = m_materialPipelines.find(shader); it != m_materialPipelines.end())
		{
			return it->second;
		}

		gpu::PipelineHandle handle{};
		if (m_gpu != nullptr && !shader.empty())
		{
			// Shared UI vertex shader + the material's custom fragment, so the fragment gets the real
			// glyph/quad geometry and font atlas and the effect is masked to the element's shapes.
			const std::string fragPath = "shaders://" + shader + ".spv";
			// A material shader provides only a fragment (it reuses the shared ui_shapes vertex), so
			// slang compiles it as a single entry point named "main"; the shared vertex .spv keeps its
			// "vertexMain" name because that file has multiple entry points.
			const gpu::GraphicsPipelineDesc desc{
			        .shaderVfsPath = "shaders://ui_shapes.spv",
			        .fragmentVfsPath = fragPath.c_str(),
			        .vertexEntry = "vertexMain",
			        .fragmentEntry = "main",
			        .colorFormat = m_colorFormat,
			        .depthFormat = gpu::Format::Undefined,
			        .depthTestEnable = false,
			        .depthWriteEnable = false,
			        .blendEnable = true,
			        .topology = gpu::PrimitiveTopology::TriangleList,
			        .polygonMode = gpu::PolygonMode::Fill,
			        .cullMode = gpu::CullMode::None,
			        .debugName = "UI.Material",
			        .descriptorHeapMappings = m_gpu->GetBindlessManager().GetDescriptorHeapMappings(),
			};
			handle = gpu::ResourceRegistry::CreateGraphicsPipeline(m_gpu->GetDevice(), desc);
			if (!handle.IsValid())
			{
				AE_ERROR(LogCategory::UI, "UiRenderer: failed to create material pipeline for '{}'", shader);
			}
		}
		m_materialPipelines.emplace(shader, handle);
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
		frame.materials.clear();
		frame.groups.clear();
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
			for (auto& [name, pipe]: m_materialPipelines)
			{
				if (pipe.IsValid())
				{
					m_effectPipelinesRetiring.push_back({pipe, static_cast<int>(kFrames) + 1});
				}
			}
			m_materialPipelines.clear();
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
			frame.effects.push_back({pipe, rect.resolvedRect, effect.params, effect.color0, effect.color1, effect.background, effect.sortOrder});
		}

		// Deterministic compositing: order effects by sortOrder (ascending = drawn earlier = underneath).
		// ECS iteration order is unspecified, so without this a per-screen overlay (e.g. the title's ink
		// drips) could draw over the screen-transition overlay instead of being swallowed by it. Stable so
		// equal-order effects keep their collection order. The background/overlay split below still applies
		// within this ordering.
		std::stable_sort(frame.effects.begin(), frame.effects.end(),
		        [](const EffectDraw& a, const EffectDraw& b) { return a.sortOrder < b.sortOrder; });

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

		BuildDrawCommands(*m_world, m_scratch, frame.materials, m_defaultFontReady ? &m_fontRegistry : nullptr, m_textures);

		const auto count = static_cast<std::uint32_t>(m_scratch.size());
		if (count == 0)
		{
			return;
		}

		// Split the command list into contiguous runs of the same shaderId. Runs with a material
		// (shaderId > 0) draw with the material's pipeline (resolved here, on the producer thread);
		// the rest use the default pipeline. A missing material pipeline falls back to default so the
		// element still renders. Commands for one element are emitted together, so its run is contiguous.
		for (std::uint32_t i = 0; i < count;)
		{
			const std::uint32_t shaderId = UiFlagsShaderId(m_scratch[i].flags);
			std::uint32_t j = i + 1;
			while (j < count && UiFlagsShaderId(m_scratch[j].flags) == shaderId)
			{
				++j;
			}
			gpu::PipelineHandle pipe = m_pipeline;
			std::uint32_t effectiveId = 0; // 0 unless a valid material pipeline was resolved
			if (shaderId != 0 && shaderId <= frame.materials.size())
			{
				const gpu::PipelineHandle mat = MaterialPipeline(frame.materials[shaderId - 1].shader);
				if (mat.IsValid())
				{
					pipe = mat;
					effectiveId = shaderId;
				}
			}
			frame.groups.push_back(DrawGroup{i, j - i, effectiveId, pipe});
			i = j;
		}

		EnsureCapacity(frame, count);
		if (!frame.buffer.IsValid())
		{
			frame.groups.clear();
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

			                // Batched UI shapes, drawn as runs split by material. Most runs use the default ui_shapes
			                // fragment; a run tagged with a UIMaterial uses that material's fragment (glyph/quad-masked).
			                // Each run offsets the command-buffer pointer to its first command (SV_InstanceID in the shared
				                // vertex is per-draw and does NOT include firstInstance, so we can't offset the draw that way).
			                for (const DrawGroup& g: frame.groups)
			                {
			                	if (g.count == 0 || !g.pipeline.IsValid())
			                	{
			                		continue;
			                	}
			                	const auto resolved = gpu::ResourceRegistry::ResolvePipeline(g.pipeline);
			                	cmd.BindPipeline(resolved.state);
			                	bindless.CmdBindHeaps(cmd);
			                	const std::uint64_t groupAddr = frame.address + static_cast<std::uint64_t>(g.first) * sizeof(UiDrawCommand);
			                	if (g.shaderId != 0 && g.shaderId <= frame.materials.size())
			                	{
			                		const UiMaterialDraw& m = frame.materials[g.shaderId - 1];
			                		const MaterialPush push{
			                		        .screenSize = {frame.extent.x, frame.extent.y, 0.f, 0.f},
			                		        .commandData = groupAddr,
			                		        .pad0 = 0,
			                		        .pad1 = 0,
			                		        .params = m.params,
			                		        .color0 = m.color0,
			                		        .color1 = m.color1,
			                		};
			                		cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                	}
			                	else
			                	{
			                		const ShapesPush push{
			                		        .screenSize = {frame.extent.x, frame.extent.y, 0.f, 0.f},
			                		        .commandData = groupAddr,
			                		        .pad0 = 0,
			                		        .pad1 = 0,
			                		};
			                		cmd.PushDataRaw(0, gpu::AsPushConstantBytes(push));
			                	}
			                	cmd.Draw(6, g.count, 0, 0);
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
