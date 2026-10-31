#include "ui/UiRenderer.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

#include "gpu/BindlessManager.hpp"
#include "gpu/CommandList.hpp"
#include "gpu/GpuDevice.hpp"
#include "gpu/TextureUpload.hpp"
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

		m_defaultFontReady = EnsureFontCurvesUploaded("Roboto");
	}

	void UiRenderer::Shutdown()
	{
		AE_PROFILE_ZONE();

		for (Frame& frame: m_frames)
		{
			for (SlotBuffer& buf: frame.buffers)
			{
				if (buf.buffer.IsValid())
				{
					gpu::ResourceRegistry::Destroy(buf.buffer);
				}
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

		// Every font's curve texture, not just the last one uploaded.
		for (auto& [name, curves]: m_fontCurveTextures)
		{
			if (curves.IsValid())
			{
				gpu::ResourceRegistry::Destroy(curves);
			}
		}
		m_fontCurveTextures.clear();
		m_fontUploadFailed.clear();

		// The cursor texture holds a TextureRegistry reference taken in AppendCursor; drop it
		// before nulling the registry pointer, or the entry's refcount never falls and the
		// cursor art outlives the renderer that was showing it.
		if (m_textures != nullptr && m_cursorTexture.IsValid())
		{
			m_textures->Release(m_cursorTexture);
		}
		m_cursorTexture = {};
		m_cursorTexturePath.clear();
		m_cursorRetryAt = {};

		m_defaultFontReady = false;
		m_textures = nullptr;
		m_upload = nullptr;
		m_gpu = nullptr;
	}

	std::uint64_t UiRenderer::FontUploadBytes(const FontAsset& font) const
	{
		if (!font.textureWidth || !font.textureHeight || font.texels.size() != std::uint64_t(font.textureWidth) * font.textureHeight) return 0;
		const gpu::TextureDesc desc{.format = gpu::Format::R32G32B32A32Sfloat, .extent = {font.textureWidth, font.textureHeight}, .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled, .aspect = gpu::ImageAspect::Color};
		return gpu::ResourceRegistry::GetTextureAllocationRequirement(desc);
	}

	Expected<UiRenderer::FontUpload> UiRenderer::BeginStreamedFontUpload(const FontAsset& font)
	{
		if (!m_upload || !m_upload->IsValid() || !font.textureWidth || !font.textureHeight || font.texels.size() != std::uint64_t(font.textureWidth) * font.textureHeight)
			AE_UNEXPECTED(AetherError::Asset("Invalid streamed font upload."));
		const gpu::TextureDesc desc{.format = gpu::Format::R32G32B32A32Sfloat, .extent = {font.textureWidth, font.textureHeight}, .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled, .aspect = gpu::ImageAspect::Color, .debugName = "UI.StreamedFontCurves"};
		FontUpload upload;
		upload.texture = gpu::ResourceRegistry::CreateTexture(desc);
		if (!upload.texture.IsValid()) AE_UNEXPECTED(AetherError::Asset("Failed to allocate streamed font texture."));
		return upload;
	}

	Expected<std::uint64_t> UiRenderer::PumpStreamedFontUpload(FontUpload& upload, const FontAsset& font, std::uint64_t maxBytes,
	        const std::function<bool(std::uint64_t)>& reserveStaging, const std::function<void(std::uint64_t)>& retireStaging)
	{
		if (upload.complete) return std::uint64_t{0};
		if (!upload.texture.IsValid() || !m_upload || upload.y >= font.textureHeight || font.texels.size() != std::uint64_t(font.textureWidth) * font.textureHeight)
			AE_UNEXPECTED(AetherError::Asset("Invalid streamed font upload state."));
		auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(font.textureWidth - upload.x, maxBytes / sizeof(glm::vec4)));
		if (!count) return std::uint64_t{0};
		std::uint64_t bytes = 0;
		std::uint64_t allocation = 0;
		gpu::MappedBufferDesc desc{.usage = gpu::BufferUsage::TransferSrc, .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu, .debugName = "UI streamed font staging"};
		while (count)
		{
			bytes = std::uint64_t(count) * sizeof(glm::vec4);
			desc.size = bytes;
			allocation = gpu::ResourceRegistry::GetMappedBufferAllocationRequirement(desc);
			if (!allocation) AE_UNEXPECTED(AetherError::Asset("Failed to query font staging allocation."));
			if (reserveStaging(allocation)) break;
			count /= 2;
		}
		if (!count) return std::uint64_t{0};
		const auto staging = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!staging.IsValid())
		{
			retireStaging(allocation);
			AE_UNEXPECTED(AetherError::Asset("Failed to allocate font staging."));
		}
		const auto mapped = gpu::ResourceRegistry::ResolveMappedBuffer(staging);
		if (!mapped.mappedPtr)
		{
			const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
			gpu::ResourceRegistry::Destroy(staging);
			gpu::ResourceRegistry::DeferDestructionCallback([retireStaging, allocation] { retireStaging(allocation); });
			AE_UNEXPECTED(AetherError::Asset("Failed to map font staging."));
		}
		std::memcpy(mapped.mappedPtr, font.texels.data() + std::uint64_t(upload.y) * font.textureWidth + upload.x, static_cast<std::size_t>(bytes));
		gpu::ResourceRegistry::FlushMappedBuffer(staging, 0, bytes);
		const gpu::ImageUploadRegion region{.width = count, .height = 1, .x = static_cast<std::int32_t>(upload.x), .y = static_cast<std::int32_t>(upload.y)};
		const bool last = upload.y + 1 == font.textureHeight && upload.x + count == font.textureWidth;
		const auto ticket = m_upload->CopyImageChunkAsyncOwned(staging, upload.texture, std::span(&region, 1), 1, !upload.submitted, last,
		        [retireStaging, allocation] { retireStaging(allocation); });
		if (!ticket) AE_UNEXPECTED(AetherError::Asset("Failed to submit font upload chunk."));
		upload.ticket = ticket;
		upload.submitted = true;
		upload.x += count;
		if (upload.x == font.textureWidth) { upload.x = 0; ++upload.y; }
		upload.complete = last;
		return bytes;
	}

	Expected<bool> UiRenderer::FinishStreamedFontUpload(std::string name, FontAsset asset, FontUpload& upload)
	{
		if (!upload.complete || !upload.texture.IsValid() || !m_upload || !m_upload->IsUploadComplete(upload.ticket))
			AE_UNEXPECTED(AetherError::Asset("Streamed font upload is not complete."));
		if (const auto* existing = m_fontRegistry.Get(name))
		{
			if (existing->curveBindlessSlot == kInvalidBindlessSlot)
				AE_UNEXPECTED(AetherError::Asset("Existing font is not GPU-resident."));
			return false;
		}
		gpu::ResourceRegistry::EnsureBindlessSampled(upload.texture, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
		const auto slot = gpu::ResourceRegistry::GetBindlessSampledSlot(upload.texture);
		if (slot == kInvalidBindlessSlot) AE_UNEXPECTED(AetherError::Asset("Failed to bind streamed font texture."));
		asset.curveBindlessSlot = slot;
		asset.texels.clear();
		asset.texels.shrink_to_fit();
		if (!m_fontRegistry.Publish(name, std::move(asset))) return false;
		m_fontCurveTextures.emplace(name, upload.texture);
		m_fontUploadFailed.erase(name);
		upload.texture = {};
		return true;
	}

	void UiRenderer::DiscardStreamedFontUpload(FontUpload& upload, std::function<void()> onGpuRetired)
	{
		if (upload.texture.IsValid())
		{
			if (upload.ticket && m_upload) m_upload->WaitForUpload(upload.ticket);
			const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
			gpu::ResourceRegistry::Destroy(upload.texture);
			if (onGpuRetired) gpu::ResourceRegistry::DeferDestructionCallback(std::move(onGpuRetired));
			upload.texture = {};
		}
		else if (onGpuRetired) onGpuRetired();
	}

	std::uint32_t UiRenderer::StreamedFontSlot(std::string_view name) const
	{
		const auto* font = m_fontRegistry.Get(name);
		return font ? font->curveBindlessSlot : kInvalidBindlessSlot;
	}

	void UiRenderer::RetireStreamedFont(std::string_view name, std::function<void()> onGpuRetired)
	{
		const std::string key(name);
		if (const auto it = m_fontCurveTextures.find(key); it != m_fontCurveTextures.end())
		{
			if (it->second.IsValid())
			{
				const std::lock_guard lock(gpu::ResourceRegistry::BufferMutex());
				gpu::ResourceRegistry::Destroy(it->second);
				if (onGpuRetired) gpu::ResourceRegistry::DeferDestructionCallback(std::move(onGpuRetired));
			}
			m_fontCurveTextures.erase(it);
		}
		if (onGpuRetired) onGpuRetired();
		m_fontUploadFailed.erase(key);
		m_fontRegistry.Invalidate(name);
	}

	void UiRenderer::EnsureFontReady(std::string_view name)
	{
		if (name.empty())
		{
			return;
		}
		// The live case, and the only one that runs most frames: the font is loaded and its
		// atlas still holds a valid bindless slot. Get() is a map lookup and never touches the
		// filesystem, so this is cheap enough to ask for every text element every frame.
		const FontAsset* font = m_fontRegistry.Get(name);
		if (font != nullptr && font->curveBindlessSlot != kInvalidBindlessSlot)
		{
			return;
		}
		// Asking again is how a font recovers - from a slot that was invalidated, or from an
		// atlas that simply had not been uploaded yet. Only an outright FAILURE is remembered,
		// so a missing font is reported once instead of once per element per frame.
		if (m_fontUploadFailed.contains(std::string(name)))
		{
			return;
		}
		if (!EnsureFontCurvesUploaded(name))
		{
			m_fontUploadFailed.emplace(name);
		}
	}

	bool UiRenderer::EnsureFontCurvesUploaded(std::string_view name)
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
		if (font->curveBindlessSlot != kInvalidBindlessSlot)
		{
			return true;
		}
		if (m_gpu == nullptr)
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: cannot upload font curves for '{}', the GPU device is unavailable", name);
			return false;
		}
		if (font->texels.empty() || font->textureWidth == 0 || font->textureHeight == 0)
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: font '{}' carries no curve data", name);
			return false;
		}

		// One RGBA32F texture per font holding every glyph's band table and curve runs. A
		// texture rather than a storage buffer because it rides the bindless heap the draw
		// command already addresses - a glyph names its font in the same field a textured
		// rect names its image, and nothing about the command layout has to change.
		const std::string debugName = "UI.FontCurves." + std::string(name);
		const gpu::Extent2D extent{font->textureWidth, font->textureHeight};
		const bool useHostCopy = gpu::CanUseHostImageCopy(gpu::Format::R32G32B32A32Sfloat, extent, 1);
		const gpu::TextureDesc desc{
		        .format = gpu::Format::R32G32B32A32Sfloat,
		        .extent = extent,
		        .usage = gpu::ImageUsage::TransferDst | gpu::ImageUsage::Sampled | (useHostCopy ? gpu::ImageUsage::HostTransfer : gpu::ImageUsage::None),
		        .aspect = gpu::ImageAspect::Color,
		        .debugName = debugName.c_str(),
		};
		const gpu::TextureHandle curves = gpu::ResourceRegistry::CreateTexture(desc);
		if (!curves.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to create the curve texture for '{}'", name);
			return false;
		}

		const gpu::ImageUploadRegion region{.width = extent.width, .height = extent.height};
		const auto uploadResult = gpu::UploadTexture(m_gpu->GetDevice(), m_gpu->GetGraphicsQueue(),
		        m_upload != nullptr ? m_upload->GetCommandPool() : nullptr, curves, std::as_bytes(std::span(font->texels)), std::span(&region, 1), useHostCopy);
		if (!uploadResult)
		{
			gpu::ResourceRegistry::Destroy(curves);
			AE_ERROR(LogCategory::UI, "UiRenderer: curve texture upload failed for '{}': {}", name, uploadResult.error().message);
			return false;
		}

		gpu::ResourceRegistry::EnsureBindlessSampled(curves, gpu::ImageAspect::Color, gpu::ImageLayout::ShaderReadOnly);
		const std::uint32_t slot = gpu::ResourceRegistry::GetBindlessSampledSlot(curves);
		if (slot == kInvalidBindlessSlot)
		{
			gpu::ResourceRegistry::Destroy(curves);
			AE_ERROR(LogCategory::UI, "UiRenderer: bindless registration failed for the curves of '{}'", name);
			return false;
		}

		font->curveBindlessSlot = slot;
		// Keyed by font name, because this map holds the only owning reference to the texture.
		// Anything already stored under this name is a previous upload for the SAME font (a
		// re-upload after its slot was invalidated), so it is destroyed rather than leaked.
		if (const auto it = m_fontCurveTextures.find(std::string(name)); it != m_fontCurveTextures.end() && it->second.IsValid())
		{
			gpu::ResourceRegistry::Destroy(it->second);
		}
		m_fontCurveTextures[std::string(name)] = curves;

		AE_INFO(LogCategory::UI, "UiRenderer: uploaded font curves '{}' ({}x{} texels, slot {})", font->curvePath, font->textureWidth, font->textureHeight, slot);
		// The payload is on the GPU now; a loaded font costs a map of metrics from here on.
		font->texels.clear();
		font->texels.shrink_to_fit();
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

	void UiRenderer::EnsureCapacity(SlotBuffer& buf, std::uint32_t count)
	{
		if (buf.capacity >= count && buf.buffer.IsValid())
		{
			return;
		}

		const std::uint32_t previousCapacity = buf.capacity;
		std::uint32_t newCapacity = buf.capacity == 0 ? kInitialCommandCapacity : buf.capacity;
		while (newCapacity < count)
		{
			newCapacity *= 2;
		}

		if (buf.buffer.IsValid())
		{
			gpu::ResourceRegistry::Destroy(buf.buffer);
			buf.buffer = {};
			buf.mapped = nullptr;
			buf.address = 0;
		}

		const gpu::MappedBufferDesc desc{
		        .size = static_cast<gpu::DeviceSize>(newCapacity) * sizeof(UiDrawCommand),
		        .usage = gpu::BufferUsage::Storage | gpu::BufferUsage::ShaderDeviceAddress,
		        .memoryUsage = gpu::MappedMemoryUsage::CpuToGpu,
		        .debugName = "UI.Commands",
		};
		buf.buffer = gpu::ResourceRegistry::CreateMappedBuffer(desc);
		if (!buf.buffer.IsValid())
		{
			AE_ERROR(LogCategory::UI, "UiRenderer: failed to allocate command buffer ({} commands)", newCapacity);
			buf.capacity = 0;
			return;
		}

		const auto view = gpu::ResourceRegistry::ResolveMappedBuffer(buf.buffer);
		buf.mapped = view.mappedPtr;
		buf.address = view.deviceAddress;
		buf.capacity = newCapacity;
		// Growing the command buffer swaps the device address every DrawGroup's commands are
		// read from. Rare in steady state; if it turns out to be happening constantly while the
		// UI churns, that is the thing to chase - so anything past the initial capacity is
		// worth a line, whether it grew into it or started there. Landing exactly ON that size
		// is not: it happens for every frame slot on every renderer reset (nine lines per play
		// cycle) and only reports the number this file already picked.
		if (newCapacity > kInitialCommandCapacity)
		{
			AE_INFO(LogCategory::UI, "UiRenderer: command buffer holds {} commands, up from {} (needed {})", newCapacity, previousCapacity, count);
		}
	}

	void UiRenderer::BuildFrame(glm::vec2 outputExtent, std::uint32_t frameSlot)
	{
		AE_PROFILE_ZONE();

		Frame& frame = m_frames[frameSlot % kFrames];
		frame.builtForSlot = frameSlot;
		frame.buildSeq = ++m_buildSeq;
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

		// Lazily upload the atlas of every font the scene draws glyphs with - project fonts
		// appear here the first frame something names them.
		//
		// Every text-bearing component is asked, not just UIText: a button label and a text box
		// each carry their own font name, so a font used ONLY by one of those was never uploaded
		// and rendered nothing at all.
		for (const auto& [enttEntity, text]: m_world->View<UIText>().each())
		{
			EnsureFontReady(text.fontName);
		}
		for (const auto& [enttEntity, button]: m_world->View<UIButton>().each())
		{
			EnsureFontReady(button.fontName);
		}
		for (const auto& [enttEntity, box]: m_world->View<UITextBox>().each())
		{
			EnsureFontReady(box.fontName);
		}

		BuildDrawCommands(*m_world, m_scratch, frame.materials, m_defaultFontReady ? &m_fontRegistry : nullptr, m_textures);
		AppendCursor();

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
			if (shaderId != 0)
			{
				// Both ways this can fail used to be silent, and both of them look the same on
				// screen: the element draws on the default pipeline, which renders its plain
				// fill instead of its material. That is the "structure turned into a flat
				// square" report, so neither is allowed to pass without saying so.
				if (shaderId > frame.materials.size())
				{
					AE_ERROR(LogCategory::UI,
					        "UiRenderer: command carries shaderId {} but the frame only has {} materials - "
					        "the material table and the command flags disagree.",
					        shaderId, frame.materials.size());
				}
				else
				{
					const gpu::PipelineHandle mat = MaterialPipeline(frame.materials[shaderId - 1].shader);
					if (mat.IsValid())
					{
						pipe = mat;
						effectiveId = shaderId;
					}
					else
					{
						AE_ERROR(LogCategory::UI,
						        "UiRenderer: no pipeline for material '{}' (shaderId {}); {} commands fall back to "
						        "the default pipeline and will draw their plain fill.",
						        frame.materials[shaderId - 1].shader, shaderId, j - i);
					}
				}
			}
			frame.groups.push_back(DrawGroup{i, j - i, effectiveId, pipe});
			i = j;
		}

		// Rotate to the next buffer in this slot's ring BEFORE writing. The one we just came
		// off may still be being read by the GPU for the last frame that used this slot; the
		// device address of that read was baked into a command buffer three frames ago and
		// nothing since has told the GPU to stop.
		frame.cursor = (frame.cursor + 1u) % Frame::kBuffersPerSlot;
		SlotBuffer& buf = frame.buffers[frame.cursor];

		EnsureCapacity(buf, count);
		if (!buf.buffer.IsValid())
		{
			frame.groups.clear();
			return;
		}

		const auto byteSize = static_cast<gpu::DeviceSize>(count) * sizeof(UiDrawCommand);
		std::memcpy(buf.mapped, m_scratch.data(), byteSize);
		gpu::ResourceRegistry::FlushMappedBuffer(buf.buffer, 0, byteSize);
		frame.mapped = buf.mapped;
		frame.address = buf.address;
		frame.count = count;
	}

	// The engine's mouse pointer, emitted after every canvas so it composites over all of them. It is
	// not an entity and belongs to no scene: a pointer that a scene load can destroy, or that another
	// canvas can sort above, is a pointer that will fail at the worst moment.
	namespace
	{
		// Above every element any canvas can produce. Layers count up from zero per canvas walk, so this
		// only has to clear a realistic element count - not be astronomically large.
		constexpr int kCursorLayer = 1 << 16;

		// How often a failed cursor-texture Acquire is retried. Every frame would log a load
		// failure per frame for a path that is simply missing; once a second still recovers the
		// cursor within a second of its art becoming loadable.
		constexpr std::chrono::seconds kCursorAcquireRetryInterval{1};
	} // namespace

	void UiRenderer::AppendCursor()
	{
		if (m_cursor == nullptr || !m_cursor->ShouldDraw() || m_textures == nullptr)
		{
			return;
		}

		// Acquire takes a reference, so the handle is cached and only re-taken when the art
		// actually changes - re-acquiring every frame would leak a reference per frame. A
		// failed Acquire returns Broken(), which IsValid() reports as usable (it routes
		// through ResolveSlot to the magenta default), so the retry gate has to name the
		// index: a handle that is not a live entry is re-attempted at most once per
		// kCursorAcquireRetryInterval, because the art may only become loadable later (a
		// project configured before its art is imported) while a retry every frame would
		// log a load failure every frame and bury real diagnostics behind one missing path.
		const CursorService::Look& look = m_cursor->GetLook();
		const bool cursorLive = m_cursorTexture.IsValid() && m_cursorTexture.index != TextureHandle::kBrokenIndex;
		const auto now = std::chrono::steady_clock::now();
		if (look.texture != m_cursorTexturePath)
		{
			if (cursorLive)
			{
				m_textures->Release(m_cursorTexture);
			}
			m_cursorTexturePath = look.texture;
			m_cursorTexture = m_textures->Acquire(m_cursorTexturePath);
			m_cursorRetryAt = now + kCursorAcquireRetryInterval;
		}
		else if (!cursorLive && now >= m_cursorRetryAt)
		{
			m_cursorTexture = m_textures->Acquire(m_cursorTexturePath);
			m_cursorRetryAt = now + kCursorAcquireRetryInterval;
		}
		if (!m_cursorTexture.IsValid())
		{
			return;
		}

		// The hotspot is the pixel of the art that sits on the mouse, so the rect is placed back from
		// the pointer by that fraction of its size - an arrow points from its corner, a pen writes from
		// its tip, and neither should have to know how the other is anchored.
		const glm::vec2 pos = m_cursor->GetPosition() - look.hotspot * look.size;

		UiDrawCommand cmd;
		cmd.type = kShapeTexturedRect;
		cmd.data0 = {pos.x, pos.y, look.size, look.size}; // data0.zw is SIZE, not the far corner
		cmd.data1 = {0.f, 0.f, 1.f, 1.f};
		cmd.color = glm::vec4(1.f);
		cmd.layer = kCursorLayer;
		cmd.textureSlot = m_textures->ResolveSlot(m_cursorTexture);
		cmd.flags = look.pixelArt ? kFlagPixelArt : 0u;
		m_scratch.push_back(cmd);
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

			                // The build side indexes this array by packet.drawSlot and the record side
			                // by ctx.frameSlot. Nothing in the types ties those together, so if they
			                // ever drift this pass draws one frame's DrawGroups - indices into the
			                // command buffer - against a different frame's buffer contents, and a
			                // material's fragment lands on whatever element happens to sit at those
			                // indices. Reported rather than asserted so an intermittent drift can be
			                // caught during real play instead of taking the editor down with it.
			                if (frame.builtForSlot != ctx.frameSlot)
			                {
				                AE_ERROR(LogCategory::UI,
				                        "UiRenderer: frame slot drift - recording slot {} but m_frames[{}] was built "
				                        "for slot {} (buildSeq {}). UI materials will draw against the wrong commands.",
				                        ctx.frameSlot, ctx.frameSlot % kFrames, frame.builtForSlot, frame.buildSeq);
			                }

			                if (frame.count == 0 && frame.effects.empty())
			                {
				                return;
			                }

			                gpu::CommandList& cmd = ctx.recorder;

			                const auto drawEffect = [&](const EffectDraw& fx)
			                {
				                const auto resolved = gpu::ResourceRegistry::ResolvePipeline(fx.pipeline);
				                cmd.BindPipeline(resolved.state);
				                bindless.CmdBindGlobalResources(cmd);

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
			                	bindless.CmdBindGlobalResources(cmd);
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
