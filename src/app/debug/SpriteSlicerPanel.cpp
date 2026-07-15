#include "debug/SpriteSlicerPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <optional>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "debug/EditorDragDrop.hpp"
#include "editor/AsepriteSpriteImporter.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"

namespace aether::editor
{
	namespace
	{
		template<std::size_t N>
		void SetBuffer(std::array<char, N>& destination, std::string_view value)
		{
			destination.fill('\0');
			const std::size_t count = std::min(value.size(), N - 1);
			std::copy_n(value.data(), count, destination.data());
		}

		[[nodiscard]] ImU32 CheckerColor(std::int32_t x, std::int32_t y)
		{
			return ((x + y) & 1) == 0 ? IM_COL32(55, 55, 55, 255) : IM_COL32(78, 78, 78, 255);
		}

		[[nodiscard]] std::string FilenameSafe(std::string_view value)
		{
			std::string result(value);
			for (char& character: result)
			{
				const unsigned char code = static_cast<unsigned char>(character);
				if (!std::isalnum(code) && character != '-' && character != '_')
				{
					character = '_';
				}
			}
			return result.empty() ? "animation" : result;
		}
	} // namespace

	void SpriteSlicerPanel::OnDetach(app::LayerContext& context)
	{
		ReleasePreview(context);
	}

	void SpriteSlicerPanel::OnImGui(app::LayerContext& context)
	{
		if (!m_visible)
		{
			return;
		}
		if (!ImGui::Begin("Sprite Slicer", &m_visible, ImGuiWindowFlags_NoScrollbar))
		{
			ImGui::End();
			return;
		}

		ImGui::SetNextItemWidth(-120.0f);
		ImGui::InputText("Source Texture", m_texturePath.data(), m_texturePath.size());
		ImGui::SameLine();
		if (ImGui::Button("Load Source"))
		{
			LoadSource(context);
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload); payload != nullptr && payload->DataSize == sizeof(dragdrop::FilePayload))
			{
				const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
				SetBuffer(m_texturePath, file->path);
				LoadSource(context);
			}
			ImGui::EndDragDropTarget();
		}

		ImGui::SetNextItemWidth(-120.0f);
		ImGui::InputText("Atlas File", m_atlasPath.data(), m_atlasPath.size());
		ImGui::SameLine();
		if (ImGui::Button("Load Atlas"))
		{
			const auto loaded = SpriteAtlasAsset::Load(std::filesystem::path(m_atlasPath.data()));
			if (loaded.has_value())
			{
				m_atlas = *loaded;
				m_textureWidth = m_atlas.textureWidth;
				m_textureHeight = m_atlas.textureHeight;
				SetBuffer(m_texturePath, m_atlas.texturePath);
				m_selectedRegion = m_atlas.sprites.empty() ? -1 : 0;
				m_status = "Atlas loaded.";
				m_statusError = false;
				LoadSource(context);
			}
			else
			{
				m_status = loaded.error().ToString();
				m_statusError = true;
			}
		}
		ImGui::SetNextItemWidth(-160.0f);
		ImGui::InputText("Aseprite JSON", m_asepritePath.data(), m_asepritePath.size());
		ImGui::SameLine();
		if (ImGui::Button("Import Aseprite"))
		{
			auto imported = ImportAsepriteSpriteMetadata(m_asepritePath.data(), m_texturePath.data(), &m_atlas);
			if (imported.has_value())
			{
				m_diagnostics = CompareSpriteAtlasReimport(m_atlas, imported->atlas);
				m_atlas = std::move(imported->atlas);
				m_selectedRegion = m_atlas.sprites.empty() ? -1 : 0;
				const std::filesystem::path atlasPath(m_atlasPath.data());
				const std::size_t clipCount = imported->animations.size();
				std::optional<std::string> clipError;
				for (SpriteAnimationAsset& animation: imported->animations)
				{
					animation.atlasPath = m_atlasPath.data();
					const std::filesystem::path animationPath = atlasPath.parent_path() / (atlasPath.stem().string() + "-" + FilenameSafe(animation.name) + ".spriteanim.toml");
					const auto saved = context.Get<SpriteAssetStore>().SaveAnimation(animationPath.string(), std::move(animation));
					if (saved.has_value())
					{
						context.Get<AssetDatabase>().Register(MakeSpriteAnimationSource(animationPath.string()), animationPath.stem().string());
					}
					else if (!clipError.has_value())
					{
						clipError = saved.error().ToString();
					}
				}
				m_status = clipError.value_or(std::format("Imported {} Aseprite regions and {} tagged clips.", m_atlas.sprites.size(), clipCount));
				m_statusError = clipError.has_value();
			}
			else
			{
				m_status = imported.error().ToString();
				m_statusError = true;
			}
		}

		if (ImGui::BeginTable("##slicerLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.68f);
			ImGui::TableSetupColumn("Authoring", ImGuiTableColumnFlags_WidthStretch, 0.32f);
			ImGui::TableNextColumn();
			DrawPreview();
			ImGui::TableNextColumn();

			const char* presets[] = {"16 px grid", "32 px grid", "64 px grid", "Custom"};
			if (ImGui::Combo("Import Preset", &m_preset, presets, static_cast<int>(std::size(presets))))
			{
				ApplyPreset(m_preset);
			}
			ImGui::DragInt2("Cell Size", &m_atlas.sliceSettings.cellWidth, 1.0f, 1, 8192);
			ImGui::DragInt2("Columns / Rows", &m_atlas.sliceSettings.columns, 1.0f, 0, 4096);
			ImGui::DragInt2("Padding", &m_atlas.sliceSettings.paddingX, 1.0f, 0, 4096);
			ImGui::DragInt2("Spacing", &m_atlas.sliceSettings.spacingX, 1.0f, 0, 4096);
			int origin = static_cast<int>(m_atlas.sliceSettings.origin);
			const char* origins[] = {"Top Left", "Bottom Left"};
			if (ImGui::Combo("Origin", &origin, origins, static_cast<int>(std::size(origins))))
			{
				m_atlas.sliceSettings.origin = static_cast<SpriteSliceOrigin>(origin);
			}
			ImGui::Checkbox("Trim Transparent Bounds", &m_atlas.sliceSettings.trimAlpha);
			int alphaThreshold = m_atlas.sliceSettings.alphaThreshold;
			if (ImGui::SliderInt("Alpha Threshold", &alphaThreshold, 0, 255))
			{
				m_atlas.sliceSettings.alphaThreshold = static_cast<std::uint8_t>(alphaThreshold);
			}
			ImGui::DragFloat("Pixels Per Unit", &m_atlas.pixelsPerUnit, 1.0f, 0.001f, 10000.0f);
			ImGui::Checkbox("Checkerboard", &m_checkerboard);
			ImGui::SameLine();
			ImGui::Checkbox("Pixel Grid", &m_showPixelGrid);

			if (ImGui::Button("Preview Re-slice", ImVec2(-1.0f, 0.0f)))
			{
				m_slicePreview = SpriteAtlasAsset::SliceGrid(m_texturePath.data(), m_textureWidth, m_textureHeight, m_atlas.sliceSettings, &m_atlas, m_rgbaPixels);
				m_diagnostics = CompareSpriteAtlasReimport(m_atlas, m_slicePreview);
				m_hasSlicePreview = true;
			}
			if (m_hasSlicePreview)
			{
				ImGui::Text("Preview: %zu regions", m_slicePreview.sprites.size());
				ImGui::TextColored(m_diagnostics.HasReferenceRisk() ? ImVec4(1.0f, 0.45f, 0.25f, 1.0f) : ImVec4(0.45f, 0.9f, 0.55f, 1.0f), "IDs: %u preserved, %u added, %u removed", m_diagnostics.preserved, m_diagnostics.added, m_diagnostics.removed);
				if (ImGui::Button("Commit Preview", ImVec2(-1.0f, 0.0f)))
				{
					m_atlas = std::move(m_slicePreview);
					m_hasSlicePreview = false;
					m_selectedRegion = m_atlas.sprites.empty() ? -1 : 0;
				}
			}

			ImGui::SeparatorText("Manual Regions");
			if (ImGui::Button("Create"))
			{
				m_atlas.texturePath = m_texturePath.data();
				m_atlas.textureWidth = m_textureWidth;
				m_atlas.textureHeight = m_textureHeight;
				auto& region = m_atlas.AddManualRegion({0, 0, std::min(32, m_textureWidth), std::min(32, m_textureHeight)}, "Sprite");
				m_atlas.RecalculateUvs(m_textureWidth, m_textureHeight);
				m_selectedRegion = static_cast<std::int32_t>(m_atlas.sprites.size() - 1);
				(void) region;
			}
			ImGui::SameLine();
			if (ImGui::Button("Duplicate") && m_selectedRegion >= 0 && m_selectedRegion < static_cast<std::int32_t>(m_atlas.sprites.size()))
			{
				const SpriteRegion source = m_atlas.sprites[static_cast<std::size_t>(m_selectedRegion)];
				SpriteRegion& duplicate = m_atlas.AddManualRegion(source.pixelRect, source.name + " Copy");
				duplicate.pivot = source.pivot;
				duplicate.border = source.border;
				duplicate.collisionOutline = source.collisionOutline;
				m_atlas.RecalculateUvs(m_textureWidth, m_textureHeight);
				m_selectedRegion = static_cast<std::int32_t>(m_atlas.sprites.size() - 1);
			}
			ImGui::SameLine();
			if (ImGui::Button("Delete") && m_selectedRegion >= 0 && m_selectedRegion < static_cast<std::int32_t>(m_atlas.sprites.size()))
			{
				m_atlas.Remove(m_atlas.sprites[static_cast<std::size_t>(m_selectedRegion)].id);
				m_selectedRegion = std::min(m_selectedRegion, static_cast<std::int32_t>(m_atlas.sprites.size()) - 1);
			}
			DrawRegionEditor();

			if (ImGui::Button("Save Atlas", ImVec2(-1.0f, 0.0f)))
			{
				m_atlas.texturePath = m_texturePath.data();
				m_atlas.textureWidth = m_textureWidth;
				m_atlas.textureHeight = m_textureHeight;
				m_atlas.RecalculateUvs(m_textureWidth, m_textureHeight);
				auto& store = context.Get<SpriteAssetStore>();
				auto saved = store.SaveAtlas(m_atlasPath.data(), m_atlas);
				if (saved.has_value())
				{
					m_diagnostics = *saved;
					context.Get<AssetDatabase>().Register(MakeSpriteAtlasSource(m_atlasPath.data()), std::filesystem::path(m_atlasPath.data()).stem().string());
					m_status = std::format("Saved {} sprites; {} IDs preserved, {} removed.", m_atlas.sprites.size(), m_diagnostics.preserved, m_diagnostics.removed);
					m_statusError = false;
				}
				else
				{
					m_status = saved.error().ToString();
					m_statusError = true;
				}
			}
			if (!m_status.empty())
			{
				ImGui::TextColored(m_statusError ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : ImVec4(0.45f, 0.9f, 0.55f, 1.0f), "%s", m_status.c_str());
			}
			ImGui::EndTable();
		}
		ImGui::End();
	}

	void SpriteSlicerPanel::LoadSource(app::LayerContext& context)
	{
		ReleasePreview(context);
		m_rgbaPixels.clear();
		const auto decoded = DecodeSpriteSourceImage(m_texturePath.data());
		if (decoded.has_value())
		{
			m_textureWidth = decoded->width;
			m_textureHeight = decoded->height;
			m_rgbaPixels = decoded->rgbaPixels;
		}

		auto& textures = context.Get<AssetManager>().GetTextureRegistry();
		m_previewTexture = textures.Acquire(m_texturePath.data());
		const std::uint32_t slot = textures.ResolveSlot(m_previewTexture);
		if (auto* imgui = context.TryGet<ImguiSubsystem>())
		{
			for (const gpu::DebugTextureInfo& info: gpu::ResourceRegistry::ListDebugTextures())
			{
				if (!info.hasBindlessSampled || info.bindlessSampledSlot != slot || info.view == nullptr)
				{
					continue;
				}
				const ImTextureID id = imgui->RegisterTexture(info.view, gpu::ImageLayout::ShaderReadOnly);
				if (id != ImTextureID_Invalid)
				{
					m_previewTextureId = static_cast<std::uint64_t>(id);
					m_textureWidth = static_cast<std::int32_t>(info.extent.width);
					m_textureHeight = static_cast<std::int32_t>(info.extent.height);
				}
				break;
			}
		}
		m_atlas.texturePath = m_texturePath.data();
		m_atlas.textureWidth = m_textureWidth;
		m_atlas.textureHeight = m_textureHeight;
		m_status = m_previewTextureId != 0 ? "Source loaded." : "Source metadata loaded; GPU preview unavailable.";
		m_statusError = false;
	}

	void SpriteSlicerPanel::ReleasePreview(app::LayerContext& context)
	{
		if (m_previewTextureId != 0)
		{
			if (auto* imgui = context.TryGet<ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_previewTextureId));
			}
			m_previewTextureId = 0;
		}
		if (m_previewTexture.IsValid())
		{
			context.Get<AssetManager>().GetTextureRegistry().Release(m_previewTexture);
			m_previewTexture = {};
		}
	}

	void SpriteSlicerPanel::DrawPreview()
	{
		ImGui::SliderFloat("Zoom", &m_zoom, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		const ImVec2 childSize = ImGui::GetContentRegionAvail();
		ImGui::BeginChild("##spriteSlicerPreview", childSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 imageSize(static_cast<float>(m_textureWidth) * m_zoom, static_cast<float>(m_textureHeight) * m_zoom);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		if (m_checkerboard)
		{
			constexpr float tile = 16.0f;
			for (std::int32_t y = 0; y < static_cast<std::int32_t>(std::ceil(imageSize.y / tile)); ++y)
			{
				for (std::int32_t x = 0; x < static_cast<std::int32_t>(std::ceil(imageSize.x / tile)); ++x)
				{
					drawList->AddRectFilled(ImVec2(origin.x + x * tile, origin.y + y * tile), ImVec2(std::min(origin.x + (x + 1) * tile, origin.x + imageSize.x), std::min(origin.y + (y + 1) * tile, origin.y + imageSize.y)), CheckerColor(x, y));
				}
			}
		}
		if (m_previewTextureId != 0)
		{
			drawList->AddImage(ImTextureRef(static_cast<ImTextureID>(m_previewTextureId)), origin, ImVec2(origin.x + imageSize.x, origin.y + imageSize.y));
		}
		const std::vector<SpriteRegion>& visibleRegions = m_hasSlicePreview ? m_slicePreview.sprites : m_atlas.sprites;
		for (std::size_t index = 0; index < visibleRegions.size(); ++index)
		{
			const SpriteRegion& region = visibleRegions[index];
			const ImVec2 min(origin.x + static_cast<float>(region.pixelRect.x) * m_zoom, origin.y + static_cast<float>(region.pixelRect.y) * m_zoom);
			const ImVec2 max(min.x + static_cast<float>(region.pixelRect.width) * m_zoom, min.y + static_cast<float>(region.pixelRect.height) * m_zoom);
			const bool selected = !m_hasSlicePreview && static_cast<std::int32_t>(index) == m_selectedRegion;
			drawList->AddRect(min, max, selected ? IM_COL32(255, 190, 55, 255) : IM_COL32(80, 210, 255, 220), 0.0f, 0, selected ? 3.0f : 1.0f);
			if (selected)
			{
				const ImVec2 pivot(min.x + region.pivot.x * (max.x - min.x), min.y + region.pivot.y * (max.y - min.y));
				drawList->AddCircleFilled(pivot, 4.0f, IM_COL32(255, 225, 70, 255));
			}
		}
		if (m_showPixelGrid && m_zoom >= 8.0f && m_textureWidth <= 1024 && m_textureHeight <= 1024)
		{
			for (std::int32_t x = 0; x <= m_textureWidth; ++x)
			{
				drawList->AddLine(ImVec2(origin.x + x * m_zoom, origin.y), ImVec2(origin.x + x * m_zoom, origin.y + imageSize.y), IM_COL32(255, 255, 255, 30));
			}
			for (std::int32_t y = 0; y <= m_textureHeight; ++y)
			{
				drawList->AddLine(ImVec2(origin.x, origin.y + y * m_zoom), ImVec2(origin.x + imageSize.x, origin.y + y * m_zoom), IM_COL32(255, 255, 255, 30));
			}
		}
		ImGui::InvisibleButton("##slicerCanvas", imageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
		if (!m_hasSlicePreview && ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const glm::vec2 pixel{(mouse.x - origin.x) / m_zoom, (mouse.y - origin.y) / m_zoom};
			m_selectedRegion = -1;
			for (std::size_t reverse = m_atlas.sprites.size(); reverse > 0; --reverse)
			{
				const SpritePixelRect& rect = m_atlas.sprites[reverse - 1].pixelRect;
				if (pixel.x >= rect.x && pixel.y >= rect.y && pixel.x < rect.x + rect.width && pixel.y < rect.y + rect.height)
				{
					m_selectedRegion = static_cast<std::int32_t>(reverse - 1);
					break;
				}
			}
		}
		if (!m_hasSlicePreview && m_selectedRegion >= 0 && m_selectedRegion < static_cast<std::int32_t>(m_atlas.sprites.size()) && ImGui::IsItemHovered() && ImGui::GetIO().KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			SpriteRegion& region = m_atlas.sprites[static_cast<std::size_t>(m_selectedRegion)];
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			region.pivot = glm::clamp(glm::vec2{(mouse.x - origin.x) / m_zoom - region.pixelRect.x, (mouse.y - origin.y) / m_zoom - region.pixelRect.y} / glm::vec2{region.pixelRect.width, region.pixelRect.height}, glm::vec2(0.0f), glm::vec2(1.0f));
		}
		ImGui::EndChild();
	}

	void SpriteSlicerPanel::DrawRegionEditor()
	{
		if (m_selectedRegion < 0 || m_selectedRegion >= static_cast<std::int32_t>(m_atlas.sprites.size()))
		{
			ImGui::TextDisabled("Select a region in the preview.");
			return;
		}
		SpriteRegion& region = m_atlas.sprites[static_cast<std::size_t>(m_selectedRegion)];
		ImGui::InputText("Name", &region.name);
		std::int32_t rect[4] = {region.pixelRect.x, region.pixelRect.y, region.pixelRect.width, region.pixelRect.height};
		if (ImGui::DragInt4("Rect X/Y/W/H", rect, 1.0f, 0, 16384))
		{
			region.pixelRect = {std::clamp(rect[0], 0, m_textureWidth - 1), std::clamp(rect[1], 0, m_textureHeight - 1), std::clamp(rect[2], 1, m_textureWidth), std::clamp(rect[3], 1, m_textureHeight)};
			region.pixelRect.width = std::min(region.pixelRect.width, m_textureWidth - region.pixelRect.x);
			region.pixelRect.height = std::min(region.pixelRect.height, m_textureHeight - region.pixelRect.y);
			m_atlas.RecalculateUvs(m_textureWidth, m_textureHeight);
		}
		ImGui::DragFloat2("Pivot", &region.pivot.x, 0.01f, 0.0f, 1.0f);
		if (ImGui::Button("Bottom Left"))
		{
			region.pivot = {0.0f, 1.0f};
		}
		ImGui::SameLine();
		if (ImGui::Button("Center"))
		{
			region.pivot = {0.5f, 0.5f};
		}
		ImGui::SameLine();
		if (ImGui::Button("Top Right"))
		{
			region.pivot = {1.0f, 0.0f};
		}
		ImGui::TextDisabled("Alt-drag in the preview to move the pivot directly.");
		ImGui::DragFloat4("Nine-slice Border", &region.border.x, 1.0f, 0.0f, 4096.0f);
		if (ImGui::Button("Box Collision Outline"))
		{
			region.collisionOutline = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear Outline"))
		{
			region.collisionOutline.clear();
		}
		ImGui::Text("Collision points: %zu", region.collisionOutline.size());
	}

	void SpriteSlicerPanel::ApplyPreset(int preset)
	{
		if (preset >= 0 && preset <= 2)
		{
			const std::int32_t size = 16 << preset;
			m_atlas.sliceSettings.cellWidth = size;
			m_atlas.sliceSettings.cellHeight = size;
			m_atlas.sliceSettings.columns = 0;
			m_atlas.sliceSettings.rows = 0;
			m_atlas.importPreset = std::format("grid-{}", size);
		}
		else
		{
			m_atlas.importPreset = "custom";
		}
	}
} // namespace aether::editor
