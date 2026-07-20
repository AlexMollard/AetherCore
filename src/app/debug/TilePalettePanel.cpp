#include "debug/TilePalettePanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/TileAssetStore.hpp"
#include "debug/Icons.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/SpriteAuthoringUi.hpp"
#include "debug/TilePaintingState.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace
	{
		// Applies one stroke direction through SetCell (revisions self-heal the
		// render caches and collision bodies).
		void ApplyStroke(TileMapAsset& map, const TilePaintStroke& stroke, bool forward)
		{
			for (const TilePaintEdit& edit: stroke.edits)
			{
				map.SetCell(edit.layer, edit.cell, forward ? edit.after : edit.before);
			}
		}
	} // namespace

	std::uint64_t TilePalettePanel::AcquirePreviewTexture(app::LayerContext& context, const std::string& texturePath)
	{
		if (const auto it = m_previews.find(texturePath); it != m_previews.end())
		{
			return it->second.imguiId;
		}
		auto& textures = context.Get<AssetManager>().GetTextureRegistry();
		const TextureHandle handle = textures.Acquire(texturePath);
		const std::uint32_t slot = textures.ResolveSlot(handle);
		Preview preview;
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
					preview.imguiId = static_cast<std::uint64_t>(id);
					preview.width = static_cast<std::int32_t>(info.extent.width);
					preview.height = static_cast<std::int32_t>(info.extent.height);
				}
				break;
			}
		}
		m_previews.emplace(texturePath, preview);
		return preview.imguiId;
	}

	void TilePalettePanel::ReleasePreviews(app::LayerContext& context)
	{
		if (auto* imgui = context.TryGet<ImguiSubsystem>())
		{
			for (const auto& [path, preview]: m_previews)
			{
				if (preview.imguiId != 0)
				{
					imgui->UnregisterTexture(static_cast<ImTextureID>(preview.imguiId));
				}
			}
		}
		m_previews.clear();
	}

	void TilePalettePanel::OnDetach(app::LayerContext& context)
	{
		ReleasePreviews(context);
	}

	void TilePalettePanel::OnImGui(app::LayerContext& context)
	{
		ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 320.0f), ImVec2(FLT_MAX, FLT_MAX));
		ImGui::SetNextWindowSize(ImVec2(380.0f, 560.0f), ImGuiCond_FirstUseEver);
		ImGui::Begin("Tile Palette", VisiblePtr());
		chrome::PanelHeader("TILE PALETTE");

		auto* state = context.TryGet<TilePaintingState>();
		auto* tiles = context.TryGet<TileAssetStore>();
		auto* selection = context.TryGet<SceneSelection>();
		World& world = context.Get<World>();
		if (state == nullptr || tiles == nullptr || selection == nullptr)
		{
			ImGui::TextDisabled("Tile services unavailable.");
			ImGui::End();
			return;
		}

		const Entity entity = selection->Primary();
		auto* component = entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity)) ? world.TryGet<TileMapComponent>(entity) : nullptr;
		if (component == nullptr)
		{
			ImGui::TextDisabled("Select an entity with a Tile Map component.");
			ImGui::TextDisabled("(Hierarchy > + > 2D > Tile Map creates one.)");
			state->tool = TileTool::None;
			ImGui::End();
			return;
		}

		// ── Assets row ────────────────────────────────────────────────────────
		if (component->tilemapPath.empty())
		{
			spriteui::DrawEmptyState(ICON_FA_IMAGE, "No tilemap asset yet", "Pick a sprite atlas below - every sprite in it becomes a paintable tile - and Create builds the tileset + tilemap under project://assets/tilemaps/.");
			const spriteui::AssetSlotChange atlasSlot = spriteui::DrawAssetSlot(context, "tileAtlas", ICON_FA_IMAGE, "Atlas", m_atlasInput, spriteui::AssetRole::Atlas, "Choose a sprite atlas...", true);
			if (atlasSlot.changed)
			{
				const std::size_t copyLength = std::min(atlasSlot.path.size(), sizeof(m_atlasInput) - 1);
				std::memcpy(m_atlasInput, atlasSlot.path.c_str(), copyLength);
				m_atlasInput[copyLength] = '\0';
			}
			ImGui::BeginDisabled(m_atlasInput[0] == '\0');
			if (chrome::PrimaryButton(ICON_FA_WAND_MAGIC_SPARKLES "  Create tileset + tilemap", ImVec2(-1.0f, 0.0f)))
			{
				auto* sprites = context.TryGet<SpriteAssetStore>();
				const auto atlas = sprites != nullptr ? sprites->LoadAtlas(m_atlasInput) : decltype(sprites->LoadAtlas(m_atlasInput)){};
				if (sprites == nullptr || !atlas.has_value())
				{
					m_status = sprites == nullptr ? "Sprite store unavailable." : atlas.error().ToString();
					m_statusError = true;
				}
				else
				{
					const std::string stem = std::filesystem::path(m_atlasInput).stem().stem().generic_string();
					const std::string baseDir = "project://assets/tilemaps/";
					TileSetAsset tileSet;
					tileSet.name = stem;
					tileSet.cellSize = 1.0f;
					for (const SpriteRegion& region: (*atlas)->sprites)
					{
						tileSet.AddTile(m_atlasInput, region.id, region.name);
					}
					TileMapAsset map;
					const std::string setPath = baseDir + stem + ".tileset.toml";
					const std::string mapPath = baseDir + stem + ".tilemap";
					map.tileSetPath = setPath;
					map.cellSize = tileSet.cellSize;
					map.layers.emplace_back();
					if (tiles->SaveTileSet(setPath, std::move(tileSet)).has_value() && tiles->SaveTileMap(mapPath, std::move(map)).has_value())
					{
						component->tilemapPath = mapPath;
						m_status = "Created " + mapPath;
						m_statusError = false;
					}
					else
					{
						m_status = "Failed to write tile assets under " + baseDir;
						m_statusError = true;
					}
				}
			}
			ImGui::EndDisabled();
			spriteui::DrawStatus(m_status, m_statusError);
			ImGui::End();
			return;
		}

		TileMapAsset* map = tiles->MutableTileMap(component->tilemapPath);
		if (map == nullptr)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Failed to load '%s'", component->tilemapPath.c_str());
			ImGui::End();
			return;
		}
		const auto tileSetResult = tiles->LoadTileSet(map->tileSetPath);
		if (!tileSetResult.has_value())
		{
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Failed to load tileset '%s'", map->tileSetPath.c_str());
			ImGui::End();
			return;
		}
		const TileSetAsset& tileSet = **tileSetResult;

		ImGui::TextDisabled("%s%s", component->tilemapPath.c_str(), state->mapDirty ? " *" : "");
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 166.0f);
		if (ImGui::SmallButton(ICON_FA_FLOPPY_DISK " Save"))
		{
			TileMapAsset copy = *map;
			if (tiles->SaveTileMap(component->tilemapPath, std::move(copy)).has_value())
			{
				state->mapDirty = false;
				m_status = "Saved.";
				m_statusError = false;
			}
			else
			{
				m_status = "Save failed.";
				m_statusError = true;
			}
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(state->undo.empty());
		if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT " Undo") && !state->undo.empty())
		{
			ApplyStroke(*map, state->undo.back(), false);
			state->redo.push_back(std::move(state->undo.back()));
			state->undo.pop_back();
			state->mapDirty = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(state->redo.empty());
		if (ImGui::SmallButton(ICON_FA_ROTATE_RIGHT " Redo") && !state->redo.empty())
		{
			ApplyStroke(*map, state->redo.back(), true);
			state->undo.push_back(std::move(state->redo.back()));
			state->redo.pop_back();
			state->mapDirty = true;
		}
		ImGui::EndDisabled();

		// Ctrl+Z / Ctrl+Y while a tool is active.
		if (state->tool != TileTool::None && ImGui::GetIO().KeyCtrl)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !state->undo.empty())
			{
				ApplyStroke(*map, state->undo.back(), false);
				state->redo.push_back(std::move(state->undo.back()));
				state->undo.pop_back();
				state->mapDirty = true;
			}
			if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && !state->redo.empty())
			{
				ApplyStroke(*map, state->redo.back(), true);
				state->undo.push_back(std::move(state->redo.back()));
				state->redo.pop_back();
				state->mapDirty = true;
			}
		}

		// ── Tools ─────────────────────────────────────────────────────────────
		ImGui::SeparatorText("Tools");
		const auto toolButton = [&](TileTool tool, const char* label, const char* tooltip)
		{
			const bool active = state->tool == tool;
			if (active)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, chrome::WithAlpha(chrome::kAccentHi, 0.55f));
			}
			if (ImGui::Button(label))
			{
				state->tool = active ? TileTool::None : tool;
			}
			if (active)
			{
				ImGui::PopStyleColor();
			}
			ImGui::SetItemTooltip("%s", tooltip);
			ImGui::SameLine();
		};
		toolButton(TileTool::Pencil, ICON_FA_PEN "##pencil", "Pencil: paint cells (drag)");
		toolButton(TileTool::Rectangle, ICON_FA_EXPAND "##rect", "Rectangle: drag to fill a rect");
		toolButton(TileTool::Fill, ICON_FA_WAND_MAGIC_SPARKLES "##fill", "Fill: flood fill (bounded)");
		toolButton(TileTool::Erase, ICON_FA_XMARK "##erase", "Erase: clear cells (drag)");
		toolButton(TileTool::Picker, ICON_FA_EYE "##picker", "Picker: sample a painted cell");
		ImGui::Checkbox("Flip X", &state->flipX);
		ImGui::SameLine();
		ImGui::Checkbox("Flip Y", &state->flipY);

		// ── Layers ────────────────────────────────────────────────────────────
		ImGui::SeparatorText("Layers");
		for (std::size_t i = 0; i < map->layers.size(); ++i)
		{
			TileMapLayer& layer = map->layers[i];
			ImGui::PushID(static_cast<int>(i));
			const bool active = state->activeLayer == i;
			if (ImGui::RadioButton("##active", active))
			{
				state->activeLayer = i;
			}
			ImGui::SetItemTooltip("Paint target");
			ImGui::SameLine();
			state->mapDirty |= ImGui::Checkbox("##visible", &layer.visible);
			ImGui::SetItemTooltip("Visible");
			ImGui::SameLine();
			state->mapDirty |= ImGui::Checkbox("##collision", &layer.collision);
			ImGui::SetItemTooltip("Collision (layer-wide: backdrop layers never collide)");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(90.0f);
			ImGui::SliderFloat("##opacity", &layer.opacity, 0.0f, 1.0f, "%.2f");
			state->mapDirty |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::SameLine();
			ImGui::TextUnformatted(layer.name.c_str());
			ImGui::SameLine(ImGui::GetContentRegionMax().x - 24.0f);
			ImGui::BeginDisabled(map->layers.size() <= 1);
			if (ImGui::SmallButton(ICON_FA_XMARK))
			{
				map->layers.erase(map->layers.begin() + static_cast<std::ptrdiff_t>(i));
				state->activeLayer = std::min(state->activeLayer, map->layers.size() - 1);
				state->mapDirty = true;
				ImGui::EndDisabled();
				ImGui::PopID();
				break;
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}
		ImGui::SetNextItemWidth(160.0f);
		ImGui::InputTextWithHint("##layerName", "new layer name", m_layerName, sizeof(m_layerName));
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_PLUS " Layer"))
		{
			TileMapLayer layer;
			layer.name = m_layerName[0] != '\0' ? m_layerName : ("Layer " + std::to_string(map->layers.size()));
			map->layers.push_back(std::move(layer));
			m_layerName[0] = '\0';
			state->mapDirty = true;
		}

		// ── Tile grid ─────────────────────────────────────────────────────────
		ImGui::SeparatorText("Tiles");
		auto* sprites = context.TryGet<SpriteAssetStore>();
		constexpr float kTileButton = 40.0f;
		const float panelWidth = ImGui::GetContentRegionAvail().x;
		const int columns = std::max(1, static_cast<int>(panelWidth / (kTileButton + 8.0f)));
		int column = 0;
		for (const TileDefinition& tile: tileSet.tiles)
		{
			ImGui::PushID(static_cast<int>(tile.id.value));
			bool drawn = false;
			if (sprites != nullptr)
			{
				if (const auto atlas = sprites->LoadAtlas(tile.atlasPath); atlas.has_value())
				{
					if (const SpriteRegion* region = (*atlas)->Find(tile.spriteId))
					{
						const std::uint64_t textureId = AcquirePreviewTexture(context, (*atlas)->texturePath);
						if (textureId != 0)
						{
							const ImVec2 uv0{region->uvRect.x, region->uvRect.y};
							const ImVec2 uv1{region->uvRect.z, region->uvRect.w};
							const bool selected = state->selectedTile == tile.id;
							if (selected)
							{
								ImGui::PushStyleColor(ImGuiCol_Button, chrome::WithAlpha(chrome::kAccentHi, 0.65f));
							}
							if (ImGui::ImageButton("##tile", ImTextureRef(static_cast<ImTextureID>(textureId)), ImVec2(kTileButton, kTileButton), uv0, uv1))
							{
								state->selectedTile = tile.id;
								if (state->tool == TileTool::None || state->tool == TileTool::Erase)
								{
									state->tool = TileTool::Pencil;
								}
							}
							if (selected)
							{
								ImGui::PopStyleColor();
							}
							drawn = true;
						}
					}
				}
			}
			if (!drawn)
			{
				if (ImGui::Button(tile.name.empty() ? "?" : tile.name.c_str(), ImVec2(kTileButton, kTileButton)))
				{
					state->selectedTile = tile.id;
				}
			}
			ImGui::SetItemTooltip("%s%s", tile.name.c_str(), tile.collision == TileCollisionKind::Full ? " (solid)" : tile.collision == TileCollisionKind::Rect ? " (rect collider)" : "");
			if (++column < columns)
			{
				ImGui::SameLine();
			}
			else
			{
				column = 0;
			}
			ImGui::PopID();
		}
		if (tileSet.tiles.empty())
		{
			ImGui::TextDisabled("Tileset has no tiles.");
		}

		// ── Selected tile: collision editing ─────────────────────────────────
		if (TileSetAsset* mutableSet = tiles->MutableTileSet(map->tileSetPath))
		{
			if (TileDefinition* tile = mutableSet->Find(state->selectedTile))
			{
				ImGui::SeparatorText("Selected Tile");
				ImGui::TextUnformatted(tile->name.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("- collision saves to the tileset");

				bool commit = false;
				int kind = static_cast<int>(tile->collision);
				constexpr const char* kKinds[] = {"None", "Full cell", "Rect"};
				ImGui::SetNextItemWidth(140.0f);
				if (ImGui::Combo("Collision", &kind, kKinds, IM_ARRAYSIZE(kKinds)))
				{
					tile->collision = static_cast<TileCollisionKind>(std::clamp(kind, 0, 2));
					commit = true;
				}
				if (tile->collision == TileCollisionKind::Rect)
				{
					// Cell fractions, y-up from the cell's bottom-left.
					ImGui::SetNextItemWidth(220.0f);
					ImGui::DragFloat4("Rect (x, y, w, h)", &tile->collisionRect.x, 0.01f, 0.0f, 1.0f, "%.3f");
					commit |= ImGui::IsItemDeactivatedAfterEdit();
					tile->collisionRect.x = std::clamp(tile->collisionRect.x, 0.0f, 1.0f);
					tile->collisionRect.y = std::clamp(tile->collisionRect.y, 0.0f, 1.0f);
					tile->collisionRect.z = std::clamp(tile->collisionRect.z, 0.01f, 1.0f - tile->collisionRect.x);
					tile->collisionRect.w = std::clamp(tile->collisionRect.w, 0.01f, 1.0f - tile->collisionRect.y);
					ImGui::SetItemTooltip("Cell fractions, y-up from the cell's bottom-left.\nContiguous tiles sharing a rect merge into one collider.");
				}
				if (commit)
				{
					TileSetAsset copy = *mutableSet;
					if (tiles->SaveTileSet(map->tileSetPath, std::move(copy)).has_value())
					{
						m_status = "Tile collision saved.";
						m_statusError = false;
					}
					else
					{
						m_status = "Tileset save failed.";
						m_statusError = true;
					}
				}
			}
		}

		if (!m_status.empty())
		{
			ImGui::Separator();
			ImGui::TextColored(m_statusError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", m_status.c_str());
		}
		ImGui::End();
	}
} // namespace aether::editor
