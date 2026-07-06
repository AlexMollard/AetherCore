#include "debug/TextureInspectorPanel.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <vector>

#include <imgui.h>

#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "debug/Icons.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "utils/FuzzyMatch.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

namespace aether::app
{
	namespace
	{
		template<typename Enum>
		bool HasFlag(Enum value, Enum flag) noexcept
		{
			using Underlying = std::underlying_type_t<Enum>;
			return (static_cast<Underlying>(value) & static_cast<Underlying>(flag)) != 0;
		}

		// Toolbar "Usage" dropdown: 0=All, then a few common buckets.
		bool PassesUsageFilter(gpu::ImageUsage usage, gpu::ImageAspect aspect, int filter)
		{
			switch (filter)
			{
				case 1:
					return HasFlag(usage, gpu::ImageUsage::Sampled);
				case 2:
					return HasFlag(usage, gpu::ImageUsage::ColorAttachment);
				case 3:
					return HasFlag(usage, gpu::ImageUsage::DepthStencilAttachment) || HasFlag(aspect, gpu::ImageAspect::Depth);
				case 4:
					return HasFlag(usage, gpu::ImageUsage::Storage);
				default:
					return true;
			}
		}

		// Sort the (already-filtered) view by the clicked column. columnId matches
		// the user id passed to TableSetupColumn.
		void SortTextures(std::vector<const gpu::DebugTextureInfo*>& list, ImGuiID columnId, bool ascending)
		{
			const auto less = [columnId](const gpu::DebugTextureInfo* a, const gpu::DebugTextureInfo* b)
			{
				switch (columnId)
				{
					case 1:
						return static_cast<std::uint64_t>(a->extent.width) * a->extent.height < static_cast<std::uint64_t>(b->extent.width) * b->extent.height;
					case 2:
						return static_cast<int>(a->format) < static_cast<int>(b->format);
					case 4:
						return a->handle.bits < b->handle.bits;
					default:
						return a->debugName < b->debugName;
				}
			};
			std::stable_sort(list.begin(), list.end(), [&](const gpu::DebugTextureInfo* a, const gpu::DebugTextureInfo* b) { return ascending ? less(a, b) : less(b, a); });
		}

		// Transparency backdrop tiled across the visible preview region.
		void DrawCheckerboard(ImDrawList* drawList, ImVec2 origin, ImVec2 size)
		{
			constexpr float cell = 12.0f;
			for (float y = 0.0f; y < size.y; y += cell)
			{
				for (float x = 0.0f; x < size.x; x += cell)
				{
					const bool dark = (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2 == 0;
					drawList->AddRectFilled(ImVec2(origin.x + x, origin.y + y), ImVec2(origin.x + std::min(x + cell, size.x), origin.y + std::min(y + cell, size.y)), dark ? IM_COL32(72, 76, 84, 255) : IM_COL32(112, 116, 124, 255));
				}
			}
		}
	} // anonymous namespace

	TextureInspectorPanel::~TextureInspectorPanel()
	{
		// Textures are released in OnDetach, which is called before destruction.
	}

	void TextureInspectorPanel::OnDetach(LayerContext& context)
	{
		ReleaseTextures(context);
	}

	void TextureInspectorPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Textures", VisiblePtr());

		const std::vector<gpu::DebugTextureInfo> textures = gpu::ResourceRegistry::ListDebugTextures();
		if (textures.empty())
		{
			ImGui::TextDisabled("No registered textures");
			ImGui::End();
			return;
		}

		// Drop cached ImGui descriptors for textures that no longer exist.
		if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
		{
			for (auto it = m_textureInspectorTextureIds.begin(); it != m_textureInspectorTextureIds.end();)
			{
				const bool stillLive = std::ranges::any_of(textures, [bits = it->first](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == bits; });
				if (!stillLive)
				{
					imgui->UnregisterTexture(static_cast<ImTextureID>(it->second));
					it = m_textureInspectorTextureIds.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		// Keep a valid selection in the full list (independent of the filter below).
		const auto selectedIt = std::ranges::find_if(textures, [this](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == m_selectedTextureBits; });
		if (selectedIt == textures.end())
		{
			m_selectedTextureBits = textures.front().handle.bits;
		}

		// ── Search + usage filter ──────────────────────────────────────────────
		ImGui::SetNextItemWidth(-170.0f);
		ImGui::InputTextWithHint("##texsearch", ICON_FA_MAGNIFYING_GLASS "  Filter textures", m_texSearch, sizeof(m_texSearch));
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160.0f);
		const char* usageFilters[] = {"All usages", "Sampled", "Color target", "Depth / Stencil", "Storage"};
		ImGui::Combo("##texusage", &m_texUsageFilter, usageFilters, static_cast<int>(std::size(usageFilters)));

		std::vector<const gpu::DebugTextureInfo*> filtered;
		filtered.reserve(textures.size());
		for (const gpu::DebugTextureInfo& candidate: textures)
		{
			if (!PassesUsageFilter(candidate.usage, candidate.aspect, m_texUsageFilter))
			{
				continue;
			}
			if (m_texSearch[0] != '\0' && !FuzzyMatch(m_texSearch, candidate.debugName).has_value())
			{
				continue;
			}
			filtered.push_back(&candidate);
		}

		// ── Sortable list ──────────────────────────────────────────────────────
		constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
		if (ImGui::BeginTable("TextureList", 5, tableFlags, ImVec2(0.0f, 200.0f)))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
			ImGui::TableSetupColumn("Extent", ImGuiTableColumnFlags_WidthFixed, 90.0f, 1);
			ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 140.0f, 2);
			ImGui::TableSetupColumn("Bindless", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 70.0f, 3);
			ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed, 70.0f, 4);
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableHeadersRow();

			if (const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs != nullptr && sortSpecs->SpecsCount > 0)
			{
				SortTextures(filtered, sortSpecs->Specs[0].ColumnUserID, sortSpecs->Specs[0].SortDirection != ImGuiSortDirection_Descending);
			}

			for (const gpu::DebugTextureInfo* entry: filtered)
			{
				const gpu::DebugTextureInfo& row = *entry;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const bool selected = row.handle.bits == m_selectedTextureBits;
				const std::string label = std::format("{}###tex{}", ShortRenderPassName(row.debugName), row.handle.bits);
				if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_selectedTextureBits = row.handle.bits;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u x %u", row.extent.width, row.extent.height);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(FormatName(row.format));
				ImGui::TableSetColumnIndex(3);
				if (row.hasBindlessSampled)
				{
					ImGui::Text("%u", row.bindlessSampledSlot);
				}
				else
				{
					ImGui::TextDisabled("-");
				}
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("0x%08X", row.handle.bits);
			}
			ImGui::EndTable();
		}
		ImGui::TextDisabled("%zu / %zu textures", filtered.size(), textures.size());

		const auto currentIt = std::ranges::find_if(textures, [this](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == m_selectedTextureBits; });
		const gpu::DebugTextureInfo& texture = (currentIt == textures.end()) ? textures.front() : *currentIt;

		// ── Preview: channel tint, fit / 1:1, zoom, checkerboard ───────────────
		ImGui::SeparatorText("Preview");
		const char* channelNames[] = {"RGBA", "Red", "Green", "Blue"};
		m_texturePreviewChannel = std::clamp(m_texturePreviewChannel, 0, static_cast<int>(std::size(channelNames)) - 1);
		ImGui::SetNextItemWidth(84.0f);
		ImGui::Combo("##channel", &m_texturePreviewChannel, channelNames, static_cast<int>(std::size(channelNames)));
		ImGui::SameLine();
		if (ImGui::SmallButton("Fit"))
		{
			m_texturePreviewFit = true;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("1:1"))
		{
			m_texturePreviewZoom = 1.0f;
			m_texturePreviewFit = false;
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(120.0f);
		if (ImGui::SliderFloat("##zoom", &m_texturePreviewZoom, 0.02f, 32.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
		{
			m_texturePreviewFit = false;
		}
		ImGui::SameLine();
		ImGui::Checkbox("Checker", &m_texturePreviewCheckerboard);

		// Channel isolation / exposure / tonemap render through the $TexturePreview
		// GPU pass when the source has a bindless slot; else fall back to a tint.
		auto* rendering = context.TryGet<RenderingSubsystem>();
		const bool shaderPreview = rendering != nullptr && texture.hasBindlessSampled && HasFlag(texture.usage, gpu::ImageUsage::Sampled) && HasFlag(texture.aspect, gpu::ImageAspect::Color);
		ImGui::SetNextItemWidth(120.0f);
		ImGui::SliderFloat("Exposure", &m_previewExposure, 0.01f, 16.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SameLine();
		ImGui::Checkbox("Tonemap", &m_previewTonemap);
		if (!shaderPreview)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(tint only: no bindless slot)");
		}
		if (rendering != nullptr)
		{
			const std::uint32_t flags = m_previewTonemap ? 1u : 0u; // bit0 = tonemap
			rendering->SetTexturePreviewRequest(shaderPreview ? texture.bindlessSampledSlot : 0xFFFFFFFFu, texture.extent, static_cast<std::uint32_t>(m_texturePreviewChannel), m_previewExposure, flags, 0u, shaderPreview);
		}

		const bool canPreview = texture.view != nullptr && HasFlag(texture.usage, gpu::ImageUsage::Sampled) && HasFlag(texture.aspect, gpu::ImageAspect::Color);
		if (canPreview)
		{
			// Shader path samples the $TexturePreview target; fallback samples the
			// source directly. UV clamps to the source's region of the fixed target.
			std::uint64_t cachedTextureId = 0;
			ImVec2 uvMax(1.0f, 1.0f);
			if (shaderPreview)
			{
				if (m_previewTextureId == 0)
				{
					if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
					{
						m_previewTextureId = static_cast<std::uint64_t>(imgui->RegisterTexture(rendering->GetTexturePreviewView(), gpu::ImageLayout::ShaderReadOnly));
					}
				}
				cachedTextureId = m_previewTextureId;
				const float edge = static_cast<float>(RenderingSubsystem::kTexturePreviewSize);
				uvMax = ImVec2(std::min(static_cast<float>(texture.extent.width), edge) / edge, std::min(static_cast<float>(texture.extent.height), edge) / edge);
			}
			else
			{
				std::uint64_t& sourceId = m_textureInspectorTextureIds[texture.handle.bits];
				if (sourceId == 0)
				{
					if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
					{
						sourceId = static_cast<std::uint64_t>(imgui->RegisterTexture(texture.view, gpu::ImageLayout::ShaderReadOnly));
					}
				}
				cachedTextureId = sourceId;
			}

			if (cachedTextureId != 0)
			{
				const float texW = static_cast<float>(texture.extent.width);
				const float texH = static_cast<float>(texture.extent.height);
				const float childHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - 150.0f);
				ImGui::BeginChild("##texpreviewregion", ImVec2(0.0f, childHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

				const ImVec2 viewRegion = ImGui::GetContentRegionAvail();
				if (m_texturePreviewFit && texW > 0.0f && texH > 0.0f)
				{
					m_texturePreviewZoom = std::clamp(std::min(viewRegion.x / texW, viewRegion.y / texH), 0.02f, 32.0f);
				}

				// Top-left of the (scrolled) content in screen space; the image is drawn here.
				const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();

				// Wheel zoom, anchored on the texel under the cursor.
				const float wheel = ImGui::GetIO().MouseWheel;
				if (ImGui::IsWindowHovered() && wheel != 0.0f && m_texturePreviewZoom > 0.0f)
				{
					const float oldZoom = m_texturePreviewZoom;
					m_texturePreviewZoom = std::clamp(m_texturePreviewZoom * std::pow(1.15f, wheel), 0.02f, 32.0f);
					m_texturePreviewFit = false;
					const ImVec2 mouse = ImGui::GetMousePos();
					const float texelX = (mouse.x - contentOrigin.x) / oldZoom;
					const float texelY = (mouse.y - contentOrigin.y) / oldZoom;
					ImGui::SetScrollX(ImGui::GetScrollX() + contentOrigin.x + texelX * m_texturePreviewZoom - mouse.x);
					ImGui::SetScrollY(ImGui::GetScrollY() + contentOrigin.y + texelY * m_texturePreviewZoom - mouse.y);
				}

				const ImVec2 imgSize(texW * m_texturePreviewZoom, texH * m_texturePreviewZoom);
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				if (m_texturePreviewCheckerboard)
				{
					DrawCheckerboard(drawList, ImGui::GetWindowPos(), ImGui::GetWindowSize());
				}
				const ImU32 tint = shaderPreview ? IM_COL32_WHITE : (m_texturePreviewChannel == 1 ? IM_COL32(255, 0, 0, 255) : m_texturePreviewChannel == 2 ? IM_COL32(0, 255, 0, 255) : m_texturePreviewChannel == 3 ? IM_COL32(0, 0, 255, 255) : IM_COL32_WHITE);
				drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(cachedTextureId)), contentOrigin, ImVec2(contentOrigin.x + imgSize.x, contentOrigin.y + imgSize.y), ImVec2(0.0f, 0.0f), uvMax, tint);
				ImGui::Dummy(imgSize); // reserve layout space so the child scrolls

				// Drag-to-pan while zoomed in.
				if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
				{
					const ImVec2 drag = ImGui::GetIO().MouseDelta;
					ImGui::SetScrollX(ImGui::GetScrollX() - drag.x);
					ImGui::SetScrollY(ImGui::GetScrollY() - drag.y);
				}

				// Pixel + UV readout under the cursor.
				bool hovering = false;
				int hoverPx = 0;
				int hoverPy = 0;
				ImVec2 hoverUv(0.0f, 0.0f);
				if (ImGui::IsWindowHovered() && imgSize.x > 0.0f && imgSize.y > 0.0f)
				{
					const ImVec2 mouse = ImGui::GetMousePos();
					hoverUv.x = std::clamp((mouse.x - contentOrigin.x) / imgSize.x, 0.0f, 1.0f);
					hoverUv.y = std::clamp((mouse.y - contentOrigin.y) / imgSize.y, 0.0f, 1.0f);
					hoverPx = static_cast<int>(hoverUv.x * texW);
					hoverPy = static_cast<int>(hoverUv.y * texH);
					hovering = true;
				}

				ImGui::EndChild();

				if (hovering)
				{
					ImGui::Text("px (%d, %d)   uv (%.3f, %.3f)   %.2fx", hoverPx, hoverPy, hoverUv.x, hoverUv.y, m_texturePreviewZoom);
				}
				else
				{
					ImGui::Text("%.0f x %.0f   %.2fx", texW, texH, m_texturePreviewZoom);
				}
			}
		}
		else
		{
			ImGui::TextDisabled("Preview unavailable for this texture");
		}

		ImGui::SeparatorText("Metadata");
		if (ImGui::BeginTable("TextureMetadata", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawMetricRow("Name", ShortRenderPassName(texture.debugName).c_str());
			DrawMetricRow("Format", FormatName(texture.format));
			DrawMetricRow("Extent", std::format("{} x {}", texture.extent.width, texture.extent.height).c_str());
			DrawMetricRow("Mips", std::format("{}", texture.mipLevels).c_str());
			DrawMetricRow("Layers", std::format("{}", texture.arrayLayers).c_str());
			DrawMetricRow("Usage", ImageUsageText(texture.usage).c_str());
			DrawMetricRow("Aspect", ImageAspectText(texture.aspect).c_str());
			DrawMetricRow("Bindless", texture.hasBindlessSampled ? std::format("{}", texture.bindlessSampledSlot).c_str() : "-");
			ImGui::EndTable();
		}

		ImGui::End();
	}

	void TextureInspectorPanel::LoadSettings(TomlConfig& config, LayerContext& /*context*/)
	{
		m_texturePreviewChannel = static_cast<int>(config.GetFloat("textureInspector.PreviewChannel", static_cast<float>(m_texturePreviewChannel)));
		m_texturePreviewZoom = config.GetFloat("textureInspector.PreviewZoom", m_texturePreviewZoom);
		m_texturePreviewCheckerboard = config.GetBool("textureInspector.PreviewCheckerboard", m_texturePreviewCheckerboard);
	}

	void TextureInspectorPanel::SaveSettings(TomlConfig& config, LayerContext& /*context*/) const
	{
		config.Set("textureInspector.PreviewChannel", static_cast<float>(m_texturePreviewChannel));
		config.Set("textureInspector.PreviewZoom", m_texturePreviewZoom);
		config.Set("textureInspector.PreviewCheckerboard", m_texturePreviewCheckerboard);
	}

	void TextureInspectorPanel::OnRenderTargetsInvalidated(LayerContext& context)
	{
		// Preview descriptors may reference render targets that were just
		// recreated; drop the whole cache so each is re-registered on demand.
		ReleaseTextures(context);
	}

	void TextureInspectorPanel::ReleaseTextures(LayerContext& context)
	{
		if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
		{
			for (const auto& [_, textureId]: m_textureInspectorTextureIds)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(textureId));
			}
			if (m_previewTextureId != 0)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_previewTextureId));
			}
		}
		m_textureInspectorTextureIds.clear();
		m_previewTextureId = 0;
	}

	const char* TextureInspectorPanel::FormatName(gpu::Format format) noexcept
	{
		switch (format)
		{
			case gpu::Format::R8Unorm:
				return "R8Unorm";
			case gpu::Format::R8G8B8A8Unorm:
				return "R8G8B8A8Unorm";
			case gpu::Format::R8G8B8A8Srgb:
				return "R8G8B8A8Srgb";
			case gpu::Format::B8G8R8A8Unorm:
				return "B8G8R8A8Unorm";
			case gpu::Format::B8G8R8A8Srgb:
				return "B8G8R8A8Srgb";
			case gpu::Format::R16G16B16A16Sfloat:
				return "R16G16B16A16Sfloat";
			case gpu::Format::R32G32Sfloat:
				return "R32G32Sfloat";
			case gpu::Format::R32G32B32Sfloat:
				return "R32G32B32Sfloat";
			case gpu::Format::R32G32B32A32Sfloat:
				return "R32G32B32A32Sfloat";
			case gpu::Format::D16Unorm:
				return "D16Unorm";
			case gpu::Format::D24UnormS8Uint:
				return "D24UnormS8Uint";
			case gpu::Format::X8D24UnormPack32:
				return "X8D24UnormPack32";
			case gpu::Format::D32Sfloat:
				return "D32Sfloat";
			case gpu::Format::D16UnormS8Uint:
				return "D16UnormS8Uint";
			case gpu::Format::D32SfloatS8Uint:
				return "D32SfloatS8Uint";
			case gpu::Format::BC4UnormBlock:
				return "BC4UnormBlock";
			case gpu::Format::BC7UnormBlock:
				return "BC7UnormBlock";
			case gpu::Format::BC7SrgbBlock:
				return "BC7SrgbBlock";
			case gpu::Format::Undefined:
			default:
				return "Undefined";
		}
	}

	std::string TextureInspectorPanel::ImageUsageText(gpu::ImageUsage usage)
	{
		std::string text;
		auto add = [&](std::string_view part)
		{
			if (!text.empty())
			{
				text += " | ";
			}
			text += part;
		};
		if (HasFlag(usage, gpu::ImageUsage::TransferSrc))
		{
			add("TransferSrc");
		}
		if (HasFlag(usage, gpu::ImageUsage::TransferDst))
		{
			add("TransferDst");
		}
		if (HasFlag(usage, gpu::ImageUsage::Sampled))
		{
			add("Sampled");
		}
		if (HasFlag(usage, gpu::ImageUsage::Storage))
		{
			add("Storage");
		}
		if (HasFlag(usage, gpu::ImageUsage::ColorAttachment))
		{
			add("ColorAttachment");
		}
		if (HasFlag(usage, gpu::ImageUsage::DepthStencilAttachment))
		{
			add("DepthStencilAttachment");
		}
		if (HasFlag(usage, gpu::ImageUsage::HostTransfer))
		{
			add("HostTransfer");
		}
		return text.empty() ? "None" : text;
	}

	std::string TextureInspectorPanel::ImageAspectText(gpu::ImageAspect aspect)
	{
		std::string text;
		auto add = [&](std::string_view part)
		{
			if (!text.empty())
			{
				text += " | ";
			}
			text += part;
		};
		if (HasFlag(aspect, gpu::ImageAspect::Color))
		{
			add("Color");
		}
		if (HasFlag(aspect, gpu::ImageAspect::Depth))
		{
			add("Depth");
		}
		if (HasFlag(aspect, gpu::ImageAspect::Stencil))
		{
			add("Stencil");
		}
		return text.empty() ? "None" : text;
	}
} // namespace aether::app
