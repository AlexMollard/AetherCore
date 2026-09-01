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
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/SpriteAuthoringUi.hpp"
#include "editor/AsepriteSpriteImporter.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"

namespace aether::editor
{
	namespace
	{
		[[nodiscard]] ImU32 CheckerColor(std::int32_t x, std::int32_t y)
		{
			return chrome::U32(((x + y) & 1) == 0 ? chrome::kPanel : chrome::kPanelHi);
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

		void DrawGridProperty(const char* label, const char* id, std::int32_t* value)
		{
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", label);
			ImGui::TableNextColumn();
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::DragInt2(id, value, 1.0f, 0, 8192);
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
		ImGui::SetNextWindowSize(ImVec2(1120.0f, 760.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Sprite Slicer", &m_visible))
		{
			ImGui::End();
			return;
		}

		DrawUnsavedAtlasPrompt(context);

		char stat[96]{};
		if (m_texturePath.empty())
		{
			std::snprintf(stat, sizeof(stat), "NO SOURCE");
		}
		else
		{
			std::snprintf(stat, sizeof(stat), "%d x %d  \xC2\xB7  %zu REGIONS", m_textureWidth, m_textureHeight, m_atlas.sprites.size());
		}
		chrome::PanelHeader("SPRITE ATLAS AUTHORING", stat);

		if (const spriteui::AssetSlotChange source = spriteui::DrawAssetSlot(context, "sourceTexture", ICON_FA_IMAGE, "Source Texture", m_texturePath, spriteui::AssetRole::Texture, "Select or drop a texture"); source.changed && !source.path.empty())
		{
			SetSource(context, source.path);
		}

		if (const spriteui::AssetSlotChange atlas = spriteui::DrawAssetSlot(context, "atlasAsset", ICON_FA_BOX_OPEN, "Atlas Asset", m_atlasPath, spriteui::AssetRole::Atlas, "Output path is derived from the texture"); atlas.changed)
		{
			if (atlas.path.empty())
			{
				m_atlasPath.clear();
			}
			else
			{
				LoadAtlas(context, atlas.path);
			}
		}

		if (m_texturePath.empty())
		{
			spriteui::DrawEmptyState(ICON_FA_IMAGE, "Choose a source texture", "Select a texture in File Explorer and click Use Selected, or drag it onto the Source Texture field. The atlas output path will be created beside it automatically.");
		}
		else if (ImGui::BeginTable("##slicerLayout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthStretch, 0.68f);
			ImGui::TableSetupColumn("Authoring", ImGuiTableColumnFlags_WidthStretch, 0.32f);
			ImGui::TableNextColumn();
			DrawPreview();

			ImGui::TableNextColumn();
			ImGui::BeginChild("##slicerAuthoring", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
			chrome::SectionTag("SLICE GRID");
			const char* presets[] = {"16 px grid", "32 px grid", "64 px grid", "Custom"};
			ImGui::SetNextItemWidth(-1.0f);
			if (ImGui::Combo("##slicePreset", &m_preset, presets, static_cast<int>(std::size(presets))))
			{
				ApplyPreset(m_preset);
			}

			if (ImGui::BeginTable("##gridProperties", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
			{
				ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 108.0f);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
				DrawGridProperty("Cell size", "##cellSize", &m_atlas.sliceSettings.cellWidth);
				DrawGridProperty("Columns / rows", "##gridCount", &m_atlas.sliceSettings.columns);
				if (ImGui::TreeNodeEx("Advanced grid settings", ImGuiTreeNodeFlags_SpanAllColumns))
				{
					DrawGridProperty("Padding", "##padding", &m_atlas.sliceSettings.paddingX);
					DrawGridProperty("Spacing", "##spacing", &m_atlas.sliceSettings.spacingX);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("Pixels / unit");
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::DragFloat("##ppu", &m_atlas.pixelsPerUnit, 1.0f, 0.001f, 10000.0f);
					ImGui::TreePop();
				}
				ImGui::EndTable();
			}

			int origin = static_cast<int>(m_atlas.sliceSettings.origin);
			const char* origins[] = {"Top Left", "Bottom Left"};
			ImGui::SetNextItemWidth(140.0f);
			if (ImGui::Combo("Origin", &origin, origins, static_cast<int>(std::size(origins))))
			{
				m_atlas.sliceSettings.origin = static_cast<SpriteSliceOrigin>(origin);
			}
			ImGui::Checkbox("Trim transparent bounds", &m_atlas.sliceSettings.trimAlpha);
			if (m_atlas.sliceSettings.trimAlpha)
			{
				int alphaThreshold = m_atlas.sliceSettings.alphaThreshold;
				ImGui::SetNextItemWidth(-1.0f);
				if (ImGui::SliderInt("Alpha threshold", &alphaThreshold, 0, 255))
				{
					m_atlas.sliceSettings.alphaThreshold = static_cast<std::uint8_t>(alphaThreshold);
				}
			}

			if (chrome::PrimaryButton(ICON_FA_WAND_MAGIC_SPARKLES "  Generate Preview", ImVec2(-1.0f, 0.0f)))
			{
				m_slicePreview = SpriteAtlasAsset::SliceGrid(m_texturePath, m_textureWidth, m_textureHeight, m_atlas.sliceSettings, &m_atlas, m_rgbaPixels);
				m_diagnostics = CompareSpriteAtlasReimport(m_atlas, m_slicePreview);
				m_hasSlicePreview = true;
			}
			if (m_hasSlicePreview)
			{
				const bool risk = m_diagnostics.HasReferenceRisk();
				ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::WithAlpha(risk ? chrome::kWarning : chrome::kSuccess, 0.08f));
				ImGui::BeginChild("##sliceDiagnostics", ImVec2(0.0f, 68.0f), ImGuiChildFlags_Borders);
				ImGui::Text("%zu regions in preview", m_slicePreview.sprites.size());
				ImGui::TextColored(risk ? chrome::kWarning : chrome::kSuccess, "%u preserved  \xC2\xB7  %u added  \xC2\xB7  %u removed", m_diagnostics.preserved, m_diagnostics.added, m_diagnostics.removed);
				ImGui::EndChild();
				ImGui::PopStyleColor();
				if (chrome::OutlineButton("Apply Preview", ImVec2(-1.0f, 0.0f)))
				{
					m_atlas = std::move(m_slicePreview);
					m_hasSlicePreview = false;
					m_selectedRegion = m_atlas.sprites.empty() ? -1 : 0;
				}
			}

			ImGui::Dummy(ImVec2(0.0f, 4.0f));
			chrome::SectionTag("REGIONS");
			if (chrome::OutlineButton(ICON_FA_PLUS "  Add"))
			{
				m_atlas.texturePath = m_texturePath;
				m_atlas.textureWidth = m_textureWidth;
				m_atlas.textureHeight = m_textureHeight;
				(void) m_atlas.AddManualRegion({0, 0, std::min(32, m_textureWidth), std::min(32, m_textureHeight)}, "Sprite");
				m_atlas.RecalculateUvs(m_textureWidth, m_textureHeight);
				m_selectedRegion = static_cast<std::int32_t>(m_atlas.sprites.size() - 1);
			}
			ImGui::SameLine();
			if (chrome::GhostButton(ICON_FA_COPY "  Duplicate") && m_selectedRegion >= 0 && m_selectedRegion < static_cast<std::int32_t>(m_atlas.sprites.size()))
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
			if (chrome::GhostButton(ICON_FA_TRASH "  Delete", ImVec2(0.0f, 0.0f), chrome::kError) && m_selectedRegion >= 0 && m_selectedRegion < static_cast<std::int32_t>(m_atlas.sprites.size()))
			{
				m_atlas.Remove(m_atlas.sprites[static_cast<std::size_t>(m_selectedRegion)].id);
				m_selectedRegion = std::min(m_selectedRegion, static_cast<std::int32_t>(m_atlas.sprites.size()) - 1);
			}
			DrawRegionEditor();

			if (ImGui::CollapsingHeader("Aseprite import"))
			{
				if (const spriteui::AssetSlotChange aseprite = spriteui::DrawAssetSlot(context, "asepriteJson", ICON_FA_FILE, "Aseprite JSON", m_asepritePath, spriteui::AssetRole::AsepriteJson, "Select or drop exported JSON", true); aseprite.changed)
				{
					m_asepritePath = aseprite.path;
					if (!m_asepritePath.empty())
					{
						ImportAseprite(context, m_asepritePath);
					}
				}
			}

			if (ImGui::CollapsingHeader("Advanced file locations"))
			{
				ImGui::InputText("Texture path", &m_texturePath);
				ImGui::InputText("Atlas path", &m_atlasPath);
				if (chrome::GhostButton("Reload texture"))
				{
					SetSource(context, m_texturePath, false);
				}
				ImGui::SameLine();
				if (chrome::GhostButton("Load atlas"))
				{
					LoadAtlas(context, m_atlasPath);
				}
			}

			ImGui::BeginDisabled(m_atlasPath.empty());
			if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save Atlas", ImVec2(-1.0f, 0.0f)))
			{
				SaveAtlas(context);
			}
			ImGui::EndDisabled();
			ImGui::EndChild();
			ImGui::EndTable();
		}

		spriteui::DrawStatus(m_status, m_statusError);
		ImGui::End();
	}

	void SpriteSlicerPanel::SetSource(app::LayerContext& context, std::string path, bool deriveAtlasPath)
	{
		if (path.empty())
		{
			return;
		}
		const bool atlasWasDerived = m_atlasPath.empty() || m_atlasPath == spriteui::CompanionPath(m_texturePath, ".spriteatlas.toml");
		ReleasePreview(context);
		m_texturePath = std::move(path);
		if (deriveAtlasPath && atlasWasDerived)
		{
			m_atlasPath = spriteui::CompanionPath(m_texturePath, ".spriteatlas.toml");
		}

		m_rgbaPixels.clear();
		const auto decoded = DecodeSpriteSourceImage(m_texturePath);
		if (!decoded.has_value())
		{
			m_status = decoded.error().ToString();
			m_statusError = true;
			return;
		}
		m_textureWidth = decoded->width;
		m_textureHeight = decoded->height;
		m_rgbaPixels = decoded->rgbaPixels;

		auto& textures = context.Get<AssetManager>().GetTextureRegistry();
		m_previewTexture = textures.Acquire(m_texturePath);
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
		m_atlas.texturePath = m_texturePath;
		m_atlas.textureWidth = m_textureWidth;
		m_atlas.textureHeight = m_textureHeight;
		m_zoom = std::clamp(520.0f / static_cast<float>(std::max(m_textureWidth, m_textureHeight)), 0.25f, 4.0f);
		context.Get<AssetDatabase>().Register(MakeTextureSource(m_texturePath), spriteui::DisplayName(m_texturePath));
		m_status = m_previewTextureId != 0 ? "Source texture ready." : "Source loaded; GPU preview is still preparing.";
		m_statusError = false;
	}

	std::string SpriteSlicerPanel::AtlasSignature() const
	{
		// Everything an author can change and would hate to lose. Cheap enough to build only
		// when a switch is requested.
		std::string signature = m_atlas.texturePath;
		signature += '|';
		signature += std::to_string(m_atlas.sprites.size());
		for (const SpriteRegion& region: m_atlas.sprites)
		{
			signature += '|';
			signature += region.name;
			signature += ':';
			signature += std::to_string(region.pixelRect.x) + ',' + std::to_string(region.pixelRect.y) + ','
			        + std::to_string(region.pixelRect.width) + ',' + std::to_string(region.pixelRect.height);
			signature += ':';
			signature += std::to_string(region.pivot.x) + ',' + std::to_string(region.pivot.y);
			signature += ':';
			signature += std::to_string(region.border.x) + ',' + std::to_string(region.border.y) + ','
			        + std::to_string(region.border.z) + ',' + std::to_string(region.border.w);
		}
		return signature;
	}

	bool SpriteSlicerPanel::AtlasDirty() const
	{
		return !m_atlasPath.empty() && AtlasSignature() != m_savedAtlasSignature;
	}

	void SpriteSlicerPanel::DrawUnsavedAtlasPrompt(app::LayerContext& context)
	{
		if (m_pendingAtlasPath.empty())
		{
			return;
		}
		ImGui::OpenPopup("Unsaved atlas##sliceSwitch");
		if (ImGui::BeginPopupModal("Unsaved atlas##sliceSwitch", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::Text("%s has unsaved regions.", spriteui::DisplayName(m_atlasPath).c_str());
			ImGui::TextDisabled("Opening another atlas will lose them.");
			ImGui::Spacing();
			if (chrome::PrimaryButton(ICON_FA_FLOPPY_DISK "  Save and open", ImVec2(150.0f, 0.0f)))
			{
				const std::string next = m_pendingAtlasPath;
				m_pendingAtlasPath.clear();
				SaveAtlas(context);
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				LoadAtlas(context, next);
				return;
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Discard", ImVec2(110.0f, 0.0f)))
			{
				const std::string next = m_pendingAtlasPath;
				m_pendingAtlasPath.clear();
				// Match the signature so the load is not challenged again.
				m_savedAtlasSignature = AtlasSignature();
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				LoadAtlas(context, next);
				return;
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Keep editing", ImVec2(130.0f, 0.0f)))
			{
				m_pendingAtlasPath.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	void SpriteSlicerPanel::LoadAtlas(app::LayerContext& context, std::string path)
	{
		if (path.empty())
		{
			return;
		}
		// Slicing an atlas is a lot of manual work and there is no undo for the file, so a
		// switch never discards it silently.
		if (AtlasDirty() && path != m_atlasPath)
		{
			m_pendingAtlasPath = path;
			return;
		}
		const auto loaded = context.Get<SpriteAssetStore>().LoadAtlas(path);
		if (!loaded.has_value())
		{
			m_status = loaded.error().ToString();
			m_statusError = true;
			return;
		}
		m_atlasPath = std::move(path);
		m_atlas = **loaded;
		m_textureWidth = m_atlas.textureWidth;
		m_textureHeight = m_atlas.textureHeight;
		m_selectedRegion = m_atlas.sprites.empty() ? -1 : 0;
		SetSource(context, m_atlas.texturePath, false);
		context.Get<AssetDatabase>().Register(MakeSpriteAtlasSource(m_atlasPath), spriteui::DisplayName(m_atlasPath));
		m_savedAtlasSignature = AtlasSignature();
		m_status = std::format("Atlas loaded with {} regions.", m_atlas.sprites.size());
		m_statusError = false;
	}

	void SpriteSlicerPanel::ImportAseprite(app::LayerContext& context, std::string path)
	{
		m_asepritePath = std::move(path);
		auto imported = ImportAsepriteSpriteMetadata(m_asepritePath, m_texturePath, &m_atlas);
		if (!imported.has_value())
		{
			m_status = imported.error().ToString();
			m_statusError = true;
			return;
		}

		m_diagnostics = CompareSpriteAtlasReimport(m_atlas, imported->atlas);
		m_atlas = std::move(imported->atlas);
		m_selectedRegion = m_atlas.sprites.empty() ? -1 : 0;
		const std::filesystem::path atlasPath(m_atlasPath);
		const std::size_t clipCount = imported->animations.size();
		std::optional<std::string> clipError;
		for (SpriteAnimationAsset& animation: imported->animations)
		{
			animation.atlasPath = m_atlasPath;
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
		m_status = clipError.value_or(std::format("Imported {} regions and {} tagged clips from Aseprite.", m_atlas.sprites.size(), clipCount));
		m_statusError = clipError.has_value();
	}

	void SpriteSlicerPanel::SaveAtlas(app::LayerContext& context)
	{
		m_atlas.texturePath = m_texturePath;
		m_atlas.textureWidth = m_textureWidth;
		m_atlas.textureHeight = m_textureHeight;
		m_atlas.RecalculateUvs(m_textureWidth, m_textureHeight);
		const auto saved = context.Get<SpriteAssetStore>().SaveAtlas(m_atlasPath, m_atlas);
		if (saved.has_value())
		{
			m_diagnostics = *saved;
			context.Get<AssetDatabase>().Register(MakeSpriteAtlasSource(m_atlasPath), spriteui::DisplayName(m_atlasPath));
			m_savedAtlasSignature = AtlasSignature();
			m_status = std::format("Saved {} regions; {} IDs preserved, {} removed.", m_atlas.sprites.size(), m_diagnostics.preserved, m_diagnostics.removed);
			m_statusError = false;
		}
		else
		{
			m_status = saved.error().ToString();
			m_statusError = true;
		}
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
		chrome::SectionTag("PREVIEW");
		ImGui::TextDisabled("%d x %d", m_textureWidth, m_textureHeight);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(130.0f);
		ImGui::SliderFloat("##zoom", &m_zoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		ImGui::SameLine();
		if (chrome::GhostButton("Fit"))
		{
			const ImVec2 available = ImGui::GetContentRegionAvail();
			m_zoom = std::clamp(std::min(available.x / static_cast<float>(std::max(m_textureWidth, 1)), std::max(200.0f, available.y) / static_cast<float>(std::max(m_textureHeight, 1))), 0.05f, 16.0f);
		}
		ImGui::SameLine();
		ImGui::Checkbox("Checker", &m_checkerboard);
		ImGui::SameLine();
		ImGui::Checkbox("Grid", &m_showPixelGrid);

		const ImVec2 childSize = ImGui::GetContentRegionAvail();
		ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::kPanel);
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
			drawList->AddRect(min, max, chrome::U32(selected ? chrome::kAccentHi : chrome::WithAlpha(chrome::kDropTarget, 0.78f)), 0.0f, 0, selected ? 3.0f : 1.0f);
			if (selected)
			{
				const ImVec2 pivot(min.x + region.pivot.x * (max.x - min.x), min.y + region.pivot.y * (max.y - min.y));
				drawList->AddCircleFilled(pivot, 4.0f, chrome::U32(chrome::kAccent));
			}
		}
		if (m_showPixelGrid && m_zoom >= 8.0f && m_textureWidth <= 1024 && m_textureHeight <= 1024)
		{
			for (std::int32_t x = 0; x <= m_textureWidth; ++x)
			{
				drawList->AddLine(ImVec2(origin.x + x * m_zoom, origin.y), ImVec2(origin.x + x * m_zoom, origin.y + imageSize.y), chrome::U32(chrome::WithAlpha(chrome::kText, 0.12f)));
			}
			for (std::int32_t y = 0; y <= m_textureHeight; ++y)
			{
				drawList->AddLine(ImVec2(origin.x, origin.y + y * m_zoom), ImVec2(origin.x + imageSize.x, origin.y + y * m_zoom), chrome::U32(chrome::WithAlpha(chrome::kText, 0.12f)));
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
		ImGui::PopStyleColor();
	}

	void SpriteSlicerPanel::DrawRegionEditor()
	{
		if (m_selectedRegion < 0 || m_selectedRegion >= static_cast<std::int32_t>(m_atlas.sprites.size()))
		{
			ImGui::TextDisabled("Click a region in the preview to edit it.");
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
		if (chrome::GhostButton("Bottom Left"))
		{
			region.pivot = {0.0f, 1.0f};
		}
		ImGui::SameLine();
		if (chrome::GhostButton("Center"))
		{
			region.pivot = {0.5f, 0.5f};
		}
		ImGui::SameLine();
		if (chrome::GhostButton("Top Right"))
		{
			region.pivot = {1.0f, 0.0f};
		}
		ImGui::TextDisabled("Alt-drag in the preview to place the pivot.");
		if (ImGui::TreeNode("Nine-slice & collision"))
		{
			ImGui::DragFloat4("Border", &region.border.x, 1.0f, 0.0f, 4096.0f);
			if (chrome::GhostButton("Box outline"))
			{
				region.collisionOutline = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
			}
			ImGui::SameLine();
			if (chrome::GhostButton("Clear outline"))
			{
				region.collisionOutline.clear();
			}
			ImGui::TextDisabled("%zu collision points", region.collisionOutline.size());
			ImGui::TreePop();
		}
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
