#include "debug/TextureInspectorPanel.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>
#include <type_traits>

#include <imgui.h>

#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
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

		ImGui::Begin("Textures");

		const std::vector<gpu::DebugTextureInfo> textures = gpu::ResourceRegistry::ListDebugTextures();
		if (textures.empty())
		{
			ImGui::TextDisabled("No registered textures");
			ImGui::End();
			return;
		}

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

		const auto selectedIt = std::ranges::find_if(textures, [this](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == m_selectedTextureBits; });
		if (selectedIt == textures.end())
		{
			m_selectedTextureBits = textures.front().handle.bits;
		}

		if (ImGui::BeginTable("TextureList", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 220.0f)))
		{
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Extent", ImGuiTableColumnFlags_WidthFixed, 90.0f);
			ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 140.0f);
			ImGui::TableSetupColumn("Bindless", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableSetupColumn("Handle", ImGuiTableColumnFlags_WidthFixed, 70.0f);
			ImGui::TableHeadersRow();

			for (const gpu::DebugTextureInfo& texture: textures)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				const bool selected = texture.handle.bits == m_selectedTextureBits;
				const std::string label = std::format("{}###tex{}", ShortRenderPassName(texture.debugName), texture.handle.bits);
				if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
				{
					m_selectedTextureBits = texture.handle.bits;
				}
				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%u x %u", texture.extent.width, texture.extent.height);
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(FormatName(texture.format));
				ImGui::TableSetColumnIndex(3);
				if (texture.hasBindlessSampled)
				{
					ImGui::Text("%u", texture.bindlessSampledSlot);
				}
				else
				{
					ImGui::TextDisabled("-");
				}
				ImGui::TableSetColumnIndex(4);
				ImGui::Text("0x%08X", texture.handle.bits);
			}
			ImGui::EndTable();
		}

		const auto currentIt = std::ranges::find_if(textures, [this](const gpu::DebugTextureInfo& texture) { return texture.handle.bits == m_selectedTextureBits; });
		if (currentIt == textures.end())
		{
			ImGui::End();
			return;
		}

		const gpu::DebugTextureInfo& texture = *currentIt;
		ImGui::SeparatorText("Preview");
		const char* channelNames[] = {"RGBA", "Red", "Green", "Blue"};
		m_texturePreviewChannel = std::clamp(m_texturePreviewChannel, 0, static_cast<int>(std::size(channelNames)) - 1);
		ImGui::Combo("Channel tint", &m_texturePreviewChannel, channelNames, static_cast<int>(std::size(channelNames)));
		ImGui::SliderFloat("Zoom", &m_texturePreviewZoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		ImGui::Checkbox("Checkerboard", &m_texturePreviewCheckerboard);

		const bool canPreview = texture.view != nullptr && HasFlag(texture.usage, gpu::ImageUsage::Sampled) && HasFlag(texture.aspect, gpu::ImageAspect::Color);
		if (canPreview)
		{
			std::uint64_t& cachedTextureId = m_textureInspectorTextureIds[texture.handle.bits];
			if (cachedTextureId == 0)
			{
				if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
				{
					cachedTextureId = static_cast<std::uint64_t>(imgui->RegisterTexture(texture.view, gpu::ImageLayout::ShaderReadOnly));
				}
			}

			if (cachedTextureId != 0)
			{
				const ImVec2 region = ImGui::GetContentRegionAvail();
				const float aspect = texture.extent.height > 0 ? static_cast<float>(texture.extent.width) / static_cast<float>(texture.extent.height) : 1.0f;
				ImVec2 previewSize(std::min(region.x, static_cast<float>(texture.extent.width) * m_texturePreviewZoom), 0.0f);
				previewSize.y = previewSize.x / aspect;
				if (previewSize.y > region.y)
				{
					previewSize.y = region.y;
					previewSize.x = previewSize.y * aspect;
				}

				if (m_texturePreviewCheckerboard)
				{
					const ImVec2 p = ImGui::GetCursorScreenPos();
					ImDrawList* drawList = ImGui::GetWindowDrawList();
					const float cell = 12.0f;
					for (float y = 0.0f; y < previewSize.y; y += cell)
					{
						for (float x = 0.0f; x < previewSize.x; x += cell)
						{
							const bool dark = (static_cast<int>(x / cell) + static_cast<int>(y / cell)) % 2 == 0;
							drawList->AddRectFilled(ImVec2(p.x + x, p.y + y), ImVec2(p.x + std::min(x + cell, previewSize.x), p.y + std::min(y + cell, previewSize.y)), dark ? IM_COL32(72, 76, 84, 255) : IM_COL32(112, 116, 124, 255));
						}
					}
				}
				const ImU32 tint = m_texturePreviewChannel == 1 ? IM_COL32(255, 0, 0, 255) : m_texturePreviewChannel == 2 ? IM_COL32(0, 255, 0, 255) : m_texturePreviewChannel == 3 ? IM_COL32(0, 0, 255, 255) : IM_COL32_WHITE;
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddImage(ImTextureRef(static_cast<ImTextureID>(cachedTextureId)), p, ImVec2(p.x + previewSize.x, p.y + previewSize.y), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
				ImGui::Dummy(previewSize);
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

	void TextureInspectorPanel::ReleaseTextures(LayerContext& context)
	{
		if (auto imgui = context.TryGet<aether::ImguiSubsystem>())
		{
			for (const auto& [_, textureId]: m_textureInspectorTextureIds)
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(textureId));
			}
		}
		m_textureInspectorTextureIds.clear();
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
