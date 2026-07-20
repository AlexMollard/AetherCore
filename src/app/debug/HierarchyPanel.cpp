#include "debug/HierarchyPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entt.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/AssetTypes.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/Renderer.hpp"
#include "debug/UndoStack.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/World.hpp"
#include "scene/SceneSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/Logger.hpp"
#include "utils/TomlConfig.hpp"
#include "utils/SettingsService.hpp"
#include "utils/Profiler.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEntities.hpp"

namespace aether::editor
{
	namespace
	{
		std::string ToLower(std::string_view s)
		{
			std::string out(s);
			std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		bool ContainsCaseInsensitive(std::string_view text, std::string_view lowerNeedle)
		{
			if (lowerNeedle.empty())
			{
				return true;
			}

			return std::search(text.begin(), text.end(), lowerNeedle.begin(), lowerNeedle.end(), [](char a, char b) { return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) == b; }) != text.end();
		}

		void ReleaseMaterialAssetTextures(AssetManager& assets, const MaterialAsset& material)
		{
			auto& textures = assets.GetTextureRegistry();
			for (const TextureHandle h: {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
			{
				if (h.IsValid())
				{
					textures.Release(h);
				}
			}
		}

		bool AssignMaterialPreset(app::LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			if (assets == nullptr)
			{
				return false;
			}
			auto loaded = assets->LoadMaterialPreset(path);
			if (!loaded)
			{
				return false;
			}
			const MaterialAsset material = loaded.value();
			MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), material);
			ReleaseMaterialAssetTextures(*assets, material);
			if (auto* db = context.TryGet<AssetDatabase>())
			{
				db->Register(MakeMaterialPresetSource(std::string(path)));
			}
			return true;
		}

		bool AssignTextureToEntity(app::LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			if (assets == nullptr)
			{
				return false;
			}
			const TextureHandle texture = assets->GetTextureRegistry().Acquire(path);
			MaterialSystem::SetAlbedoTexture(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), texture);
			if (texture.IsValid())
			{
				assets->GetTextureRegistry().Release(texture);
			}
			return true;
		}

		Entity InstantiatePrefabAsset(app::LayerContext& context, World& world, const std::string& name, Entity parent = {})
		{
			if (const auto prefab = app::scene::ReadPrefabFile(name))
			{
				const Entity root = app::scene::InstantiatePrefab(*prefab, world, app::scene::MakeApplySceneDeps(context.services), glm::mat4(1.0f));
				if (root.IsValid() && parent.IsValid())
				{
					ecs::SetParent(world, root, parent);
				}
				return root;
			}
			return {};
		}

		bool AssignModelToEntity(app::LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			auto* sceneCtx = context.TryGet<app::scripting::SceneContext>();
			if (assets == nullptr || sceneCtx == nullptr)
			{
				return false;
			}
			if (const auto* project = context.TryGet<app::EditorProjectContext>())
			{
				std::string bakeError;
				if (!editor::EnsureModelBaked(std::string(path), *project, bakeError))
				{
					AE_WARN(LogCategory::App, "Model import failed for '{}': {}", path, bakeError);
					return false;
				}
			}
			const bool ok = app::scene::AssignModelToEntity(world, *assets, *sceneCtx, entity, std::string(path));
			if (ok)
			{
				if (auto* db = context.TryGet<AssetDatabase>())
				{
					app::scene::RegisterModelAssets(*db, *assets, *sceneCtx, std::string(path));
				}
			}
			return ok;
		}

		bool ApplyFilePayloadToEntity(app::LayerContext& context, World& world, SceneSelection& selection, Entity entity, const dragdrop::FilePayload& payload)
		{
			switch (payload.kind)
			{
				case dragdrop::FileKind::Model:
					if (AssignModelToEntity(context, world, entity, payload.path))
					{
						selection.Select(entity);
						return true;
					}
					return false;
				case dragdrop::FileKind::Prefab:
				{
					const Entity root = InstantiatePrefabAsset(context, world, payload.path, entity);
					if (root.IsValid())
					{
						selection.Select(root);
						return true;
					}
					return false;
				}
				case dragdrop::FileKind::Material:
					return AssignMaterialPreset(context, world, entity, payload.path);
				case dragdrop::FileKind::Texture:
					return AssignTextureToEntity(context, world, entity, payload.path);
				case dragdrop::FileKind::Unknown:
				case dragdrop::FileKind::Script:
				case dragdrop::FileKind::Scene:
				default:
					return false;
			}
		}

		bool CanApplyFilePayloadInHierarchy(const dragdrop::FilePayload& payload)
		{
			return payload.kind == dragdrop::FileKind::Prefab || payload.kind == dragdrop::FileKind::Material || payload.kind == dragdrop::FileKind::Texture || payload.kind == dragdrop::FileKind::Model;
		}

		void FilterChip(const char* icon, const char* tooltip, const ImVec4& accent, bool& state)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, state ? accent : ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
			if (ImGui::SmallButton(icon))
			{
				state = !state;
			}
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", tooltip);
			}
		}

		void DrawEntityDragPreview(const World& world, Entity e)
		{
			const KindBadge badge = EntityKindBadge(world, e);
			const std::string name = EntityDisplayName(world, e);
			const std::string idText = "  #" + std::to_string(e.id);

			const ImVec2 padding(10.0f, 7.0f);
			const ImVec2 gap(7.0f, 0.0f);
			const ImVec2 mouse = ImGui::GetMousePos();
			const ImVec2 iconSize = ImGui::CalcTextSize(badge.icon);
			const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
			const ImVec2 idSize = ImGui::CalcTextSize(idText.c_str());
			const float height = std::max(ImGui::GetFrameHeight(), nameSize.y) + padding.y * 2.0f;
			const float width = padding.x * 2.0f + iconSize.x + gap.x + nameSize.x + idSize.x;
			const ImVec2 min(mouse.x + 16.0f, mouse.y + 18.0f);
			const ImVec2 max(min.x + width, min.y + height);
			const ImVec2 textPos(min.x + padding.x, min.y + (height - nameSize.y) * 0.5f);

			ImDrawList* drawList = ImGui::GetForegroundDrawList();
			drawList->AddRectFilled(ImVec2(min.x + 2.0f, min.y + 3.0f), ImVec2(max.x + 2.0f, max.y + 3.0f), IM_COL32(0, 0, 0, 95), 5.0f);
			drawList->AddRectFilled(min, max, chrome::U32(chrome::kDragGhostBg), 5.0f);
			drawList->AddRect(min, max, chrome::U32(chrome::kDragGhostBorder), 5.0f, 0, 1.0f);
			drawList->AddText(textPos, ImGui::ColorConvertFloat4ToU32(badge.color), badge.icon);
			drawList->AddText(ImVec2(textPos.x + iconSize.x + gap.x, textPos.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
			drawList->AddText(ImVec2(textPos.x + iconSize.x + gap.x + nameSize.x, textPos.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), idText.c_str());
		}

		std::vector<Entity> CollectSelectionRoots(World& world, const SceneSelection& selection)
		{
			std::vector<Entity> roots;
			roots.reserve(selection.All().size());
			auto& reg = world.GetRegistry();
			for (const Entity e: selection.All())
			{
				if (!reg.valid(World::ToEntt(e)))
				{
					continue;
				}
				bool ancestorSelected = false;
				Entity cur = e;
				while (true)
				{
					const auto* h = world.TryGet<HierarchyComponent>(cur);
					if (h == nullptr || !h->parent.IsValid())
					{
						break;
					}
					cur = h->parent;
					if (selection.Contains(cur))
					{
						ancestorSelected = true;
						break;
					}
				}
				if (!ancestorSelected)
				{
					roots.push_back(e);
				}
			}
			return roots;
		}

		void CreatePrimitive(app::LayerContext& context, World& world, SceneSelection& selection, PrimitiveMesh kind, const char* name, const char* kindName)
		{
			auto* primitives = context.TryGet<PrimitiveMeshes>();
			auto* assets = context.TryGet<AssetManager>();
			if (!primitives || !assets)
			{
				return;
			}
			const Entity e = world.Create();
			world.Emplace<NameComponent>(e, NameComponent{.name = name});
			world.Emplace<TransformComponent>(e);
			world.Emplace<MeshComponent>(e, MeshComponent{.mesh = &primitives->Get(kind)});
			world.Emplace<MeshSourceComponent>(e, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = kindName, .primitiveIndex = 0});
			MaterialAsset asset{};
			asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.f);
			asset.roughnessFactor = 0.6f;
			asset.metallicFactor = 0.0f;
			asset.doubleSided = true;
			MaterialSystem::AssignMaterial(world, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			selection.Select(e);
		}

		void ApplySpriteRegion(SpriteRendererComponent& renderer, const SpriteAtlasAsset& atlas, const SpriteRegion& region, std::string_view atlasPath)
		{
			renderer.texturePath = atlas.texturePath;
			renderer.atlasPath = atlasPath;
			renderer.spriteId = region.id;
			renderer.uvRect = region.uvRect;
			renderer.pixelSize = region.pixelSize;
			renderer.pivot = region.pivot;
			renderer.pixelsPerUnit = atlas.pixelsPerUnit;
			renderer.visible = true;
		}

		Entity CreateSpriteEntity(app::LayerContext& context, World& world, SceneSelection& selection, bool animated)
		{
			const std::string selectedPath = selection.HasAsset() ? selection.SelectedAsset().path : std::string{};
			const std::string lowerPath = ToLower(selectedPath);
			const Entity entity = world.Create();
			world.Emplace<NameComponent>(entity, NameComponent{.name = animated ? "Animated Sprite" : "Sprite"});
			world.Emplace<TransformComponent>(entity);
			auto& renderer = world.Emplace<SpriteRendererComponent>(entity);
			SpriteAnimatorComponent* animator = animated ? &world.Emplace<SpriteAnimatorComponent>(entity) : nullptr;

			if (auto* sprites = context.TryGet<SpriteAssetStore>(); sprites != nullptr && !selectedPath.empty())
			{
				std::string atlasPath;
				AssetObjectId spriteId{};
				if (lowerPath.ends_with(".spriteanim.toml"))
				{
					if (const auto loaded = sprites->LoadAnimation(selectedPath); loaded.has_value())
					{
						const SpriteAnimationAsset& animation = **loaded;
						atlasPath = animation.atlasPath;
						if (!animation.frames.empty())
						{
							spriteId = animation.frames.front().spriteId;
						}
						if (animator != nullptr)
						{
							animator->animationPath = selectedPath;
						}
					}
				}
				else if (lowerPath.ends_with(".spriteatlas.toml"))
				{
					atlasPath = selectedPath;
				}

				if (!atlasPath.empty())
				{
					if (const auto loaded = sprites->LoadAtlas(atlasPath); loaded.has_value())
					{
						const SpriteAtlasAsset& atlas = **loaded;
						const SpriteRegion* region = spriteId.IsValid() ? atlas.Find(spriteId) : (atlas.sprites.empty() ? nullptr : &atlas.sprites.front());
						if (region != nullptr)
						{
							ApplySpriteRegion(renderer, atlas, *region, atlasPath);
						}
					}
				}
			}
			if (renderer.texturePath.empty() && (lowerPath.ends_with(".png") || lowerPath.ends_with(".jpg") || lowerPath.ends_with(".jpeg") || lowerPath.ends_with(".bmp") || lowerPath.ends_with(".tga")))
			{
				renderer.texturePath = selectedPath;
			}

			selection.Select(entity);
			return entity;
		}

		Entity FindOrCreateCanvas(World& world)
		{
			Entity firstCanvas{};
			world.View<ui::UICanvas>().each(
			        [&](entt::entity canvasEntity, ui::UICanvas&)
			        {
				        if (!firstCanvas.IsValid())
				        {
					        firstCanvas = World::FromEntt(canvasEntity);
				        }
			        });
			return firstCanvas.IsValid() ? firstCanvas : ui::CreateCanvasEntity(world);
		}
	} // namespace

	void HierarchyPanel::DrawRowBackdrop(const SceneSelection& selection, Entity e, int rowIndex)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 rowMin = ImGui::GetCursorScreenPos();
		const ImVec2 rowMax = ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ImGui::GetFrameHeight());
		const double now = ImGui::GetTime();

		if ((rowIndex & 1) == 1)
		{
			drawList->AddRectFilled(rowMin, rowMax, chrome::U32(chrome::WithAlpha(chrome::kText, 0.02f)));
		}

		if (selection.Contains(e))
		{
			drawList->AddRectFilled(rowMin, rowMax, chrome::U32(chrome::kSelectionBg));
			drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), chrome::U32(chrome::kSelectionBar));
		}

		if (const auto it = m_spawnFlash.find(e.id); it != m_spawnFlash.end())
		{
			const float t = static_cast<float>((now - it->second) / 0.75);
			if (t < 1.0f)
			{
				const float eased = (1.0f - t) * (1.0f - t);
				drawList->AddRectFilled(rowMin, rowMax, chrome::U32(chrome::WithAlpha(chrome::kSuccess, eased * 0.30f)));
			}
		}

		if (m_pulseStart >= 0.0 && selection.Contains(e))
		{
			const float t = static_cast<float>((now - m_pulseStart) / 0.20);
			if (t < 1.0f)
			{
				const float eased = (1.0f - t) * (1.0f - t);
				drawList->AddRectFilled(rowMin, rowMax, chrome::U32(chrome::WithAlpha(chrome::kAccent, eased * 0.35f)));
			}
		}
	}

	void HierarchyPanel::BeginRename(const World& world, Entity e)
	{
		m_renaming = e;
		m_renameFocusPending = true;
		std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", EntityDisplayName(world, e));
	}

	void HierarchyPanel::HandleRowClick(SceneSelection& selection, Entity e)
	{
		const ImGuiIO& io = ImGui::GetIO();
		const bool inMultiSelection = selection.Contains(e) && selection.All().size() > 1;

		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
		{
			if (!io.KeyCtrl && !io.KeyShift && inMultiSelection)
			{
				// Pressing a row that is part of a multi-selection must NOT
				m_pendingCollapse = e;
				m_pendingClick = {};
			}
			else
			{
				m_pendingClick = e;
				m_pendingClickCtrl = io.KeyCtrl;
				m_pendingClickShift = io.KeyShift;
				m_pendingCollapse = {};
			}
		}

		const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
		const bool becameDrag = ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 4.0f) || ImGui::IsDragDropActive();
		if (m_pendingClick == e && released)
		{
			if (ImGui::IsItemHovered() && !becameDrag)
			{
				if (m_pendingClickCtrl)
				{
					selection.ToggleSelection(e);
					m_rangeAnchor = e;
				}
				else if (m_pendingClickShift && m_rangeAnchor.IsValid())
				{
					const auto& rows = m_rowsPrev;
					const auto ia = std::find(rows.begin(), rows.end(), m_rangeAnchor);
					const auto ib = std::find(rows.begin(), rows.end(), e);
					if (ia != rows.end() && ib != rows.end())
					{
						const auto lo = std::min(ia, ib);
						const auto hi = std::max(ia, ib);
						selection.Clear();
						for (auto it = lo; it != hi + 1; ++it)
						{
							selection.AddToSelection(*it);
						}
					}
					else
					{
						selection.Select(e);
						m_rangeAnchor = e;
					}
				}
				else if (!inMultiSelection)
				{
					selection.Select(e);
					m_rangeAnchor = e;
				}
			}
			m_pendingClick = {};
		}

		if (m_pendingCollapse == e && released)
		{
			if (inMultiSelection && !io.KeyCtrl && !io.KeyShift && ImGui::IsItemHovered() && !becameDrag)
			{
				selection.Select(e);
				m_rangeAnchor = e;
			}
			m_pendingCollapse = {};
		}
	}

	void HierarchyPanel::HandleRowDragDrop(app::LayerContext& context, World& world, SceneSelection& selection, Entity e, float dropMinY, float dropMaxY, float visualMaxX)
	{
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip))
		{
			m_pendingClick = {};
			m_pendingCollapse = {};
			ImGui::SetDragDropPayload(dragdrop::kEntityPayload, &e.id, sizeof(e.id));
			DrawEntityDragPreview(world, e);
			ImGui::EndDragDropSource();
		}
		const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const ImRect dropRect(itemRect.Min, ImVec2(visualMaxX, itemRect.Max.y));
		const ImRect extendedDropRect(ImVec2(dropRect.Min.x, dropMinY), ImVec2(dropRect.Max.x, dropMaxY));
		if (ImGui::BeginDragDropTargetCustom(extendedDropRect, ImGui::GetID("##rowDropTarget")))
		{
			const float relY = ImGui::GetMousePos().y - extendedDropRect.Min.y;
			const float rowH = extendedDropRect.GetHeight();

			DropZone zone{};
			if (ImGui::GetIO().KeyCtrl)
			{
				zone = DropZone::Inside;
			}
			else if (relY < rowH * 0.5f)
			{
				zone = DropZone::Before;
			}
			else
			{
				zone = DropZone::After;
			}

			Entity dragged{};
			bool hasEntityPayload = false;
			if (const ImGuiPayload* activePayload = ImGui::GetDragDropPayload(); activePayload && activePayload->IsDataType(dragdrop::kEntityPayload) && activePayload->DataSize == sizeof(std::uint32_t))
			{
				dragged = Entity{*static_cast<const std::uint32_t*>(activePayload->Data)};
				hasEntityPayload = true;
			}
			const bool hasScriptPayload = []()
			{
				const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
				return activePayload != nullptr && activePayload->IsDataType(dragdrop::kScriptPayload) && activePayload->DataSize == sizeof(dragdrop::ScriptPayload);
			}();
			const bool hasFilePayload = []()
			{
				const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
				if (activePayload == nullptr || !activePayload->IsDataType(dragdrop::kFilePayload) || activePayload->DataSize != sizeof(dragdrop::FilePayload))
				{
					return false;
				}
				return CanApplyFilePayloadInHierarchy(*static_cast<const dragdrop::FilePayload*>(activePayload->Data));
			}();

			const auto* targetHierarchy = world.TryGet<HierarchyComponent>(e);
			const Entity targetParent = targetHierarchy ? targetHierarchy->parent : Entity{};
			const bool canParentHere = !hasEntityPayload || (dragged != e && !ecs::IsAncestor(world, e, dragged));
			const bool canReorderHere = !hasEntityPayload || (dragged != e && (!targetParent.IsValid() || (targetParent != dragged && !ecs::IsAncestor(world, targetParent, dragged))));
			const bool canDropHere = zone == DropZone::Inside ? canParentHere : canReorderHere;

			if (ImGui::IsDragDropActive() && (hasScriptPayload || hasFilePayload || canDropHere))
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImU32 lineCol = chrome::U32(chrome::kDropTarget);
				const ImU32 fillCol = chrome::U32(chrome::kDropTargetBg);
				if (hasScriptPayload || hasFilePayload)
				{
					drawList->AddRectFilled(dropRect.Min, dropRect.Max, fillCol);
					drawList->AddRectFilled(dropRect.Min, ImVec2(dropRect.Min.x + 3.0f, dropRect.Max.y), lineCol);
				}
				else if (zone == DropZone::Before)
				{
					drawList->AddLine(ImVec2(dropRect.Min.x, dropRect.Min.y), ImVec2(dropRect.Max.x, dropRect.Min.y), lineCol, 2.0f);
				}
				else if (zone == DropZone::After)
				{
					drawList->AddLine(ImVec2(dropRect.Min.x, dropRect.Max.y), ImVec2(dropRect.Max.x, dropRect.Max.y), lineCol, 2.0f);
				}
				else
				{
					drawList->AddRectFilled(dropRect.Min, dropRect.Max, fillCol);
					drawList->AddRectFilled(dropRect.Min, ImVec2(dropRect.Min.x + 3.0f, dropRect.Max.y), lineCol);
				}
			}

			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
			{
				if (p->DataSize == sizeof(dragdrop::FilePayload))
				{
					const auto* file = static_cast<const dragdrop::FilePayload*>(p->Data);
					if (CanApplyFilePayloadInHierarchy(*file))
					{
						ApplyFilePayloadToEntity(context, world, selection, e, *file);
					}
				}
			}

			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
			{
				if (p->DataSize == sizeof(dragdrop::ScriptPayload))
				{
					const auto* script = static_cast<const dragdrop::ScriptPayload*>(p->Data);
					AddScriptToEntity(world, e, script->typeName);
				}
			}

			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(dragdrop::kEntityPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
			{
				const auto draggedId = *static_cast<const std::uint32_t*>(p->Data);
				const Entity draggedEntity{draggedId};
				const bool canParentOnRelease = draggedEntity != e && !ecs::IsAncestor(world, e, draggedEntity);
				const bool canReorderOnRelease =
				        draggedEntity != e
				        && (!targetParent.IsValid()
				                || (targetParent != draggedEntity && !ecs::IsAncestor(world, targetParent, draggedEntity))); // NOLINT(readability-suspicious-call-argument): tests whether the dragged entity is an ancestor of the prospective parent.
				if ((zone == DropZone::Inside && canParentOnRelease) || (zone != DropZone::Inside && canReorderOnRelease))
				{
					m_pendingReparent = PendingReparent{draggedEntity, e, zone};
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	bool HierarchyPanel::DrawRowContextMenu(World& world, SceneSelection& selection, Entity e)
	{
		if (!ImGui::BeginPopupContextItem("ctx"))
		{
			return false;
		}
		bool destroyed = false;
		if (ImGui::MenuItem(ICON_FA_PEN "  Rename"))
		{
			selection.Select(e);
			BeginRename(world, e);
		}
		if (ImGui::MenuItem(ICON_FA_PLUS "  Create child"))
		{
			const Entity child = world.Create();
			world.Emplace<NameComponent>(child, NameComponent{.name = "Entity"});
			ecs::SetParent(world, child, e);
			selection.Select(child);
		}
		if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate", "Ctrl+D"))
		{
			if (!selection.Contains(e))
			{
				selection.Select(e);
			}
			m_pendingDuplicate = true;
		}
		if (ImGui::MenuItem(ICON_FA_CLONE "  Copy", "Ctrl+C"))
		{
			if (!selection.Contains(e))
			{
				selection.Select(e);
			}
			m_pendingCopy = true;
		}
		if (ImGui::MenuItem(ICON_FA_CLONE "  Cut", "Ctrl+X"))
		{
			if (!selection.Contains(e))
			{
				selection.Select(e);
			}
			m_pendingCut = true;
		}
		if (ImGui::MenuItem(ICON_FA_CLONE "  Paste", "Ctrl+V"))
		{
			m_pendingPaste = true;
		}
		if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Save as Prefab"))
		{
			m_prefabSaveTarget = e;
			std::snprintf(m_prefabNameBuf, sizeof(m_prefabNameBuf), "%s", EntityDisplayName(world, e));
			for (char* c = m_prefabNameBuf; *c != '\0'; ++c)
			{
				*c = *c == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
			}
			m_openPrefabSave = true;
		}
		ImGui::Separator();
		const bool selfDisabled = world.Has<DisabledComponent>(e);
		if (ImGui::MenuItem(selfDisabled ? ICON_FA_POWER_OFF "  Enable" : ICON_FA_POWER_OFF "  Disable"))
		{
			if (selfDisabled)
			{
				world.Remove<DisabledComponent>(e);
			}
			else
			{
				world.EmplaceOrReplace<DisabledComponent>(e);
			}
			m_dirty = true;
		}
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FA_SITEMAP "  Select children"))
		{
			if (const auto* h = world.TryGet<HierarchyComponent>(e))
			{
				selection.Clear();
				for (const Entity c: h->children)
				{
					selection.AddToSelection(c);
				}
			}
		}
		if (ImGui::MenuItem(ICON_FA_SITEMAP "  Select siblings"))
		{
			const auto* h = world.TryGet<HierarchyComponent>(e);
			const Entity parent = h ? h->parent : Entity{};
			if (parent.IsValid())
			{
				const auto* ph = world.TryGet<HierarchyComponent>(parent);
				if (ph)
				{
					selection.Clear();
					for (const Entity c: ph->children)
					{
						selection.AddToSelection(c);
					}
				}
			}
			else
			{
				selection.Clear();
				for (const Entity root: world.Roots())
				{
					if (root.IsValid() && world.GetRegistry().valid(World::ToEntt(root)))
					{
						selection.AddToSelection(root);
					}
				}
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_FA_TRASH "  Delete (subtree)"))
		{
			ecs::DestroyHierarchy(world, e);
			selection.Clear();
			destroyed = true;
		}
		ImGui::EndPopup();
		return destroyed;
	}

	void HierarchyPanel::DrawRowContent(World& world, Entity e, bool searching, std::string_view needle, bool continuePreviousItem)
	{
		const KindBadge badge = EntityKindBadge(world, e);
		ImGui::AlignTextToFramePadding();
		if (continuePreviousItem)
		{
			ImGui::SameLine();
		}

		const bool inactive = ecs::HasDisabledAncestor(world, e);
		ImVec4 iconColor = badge.color;
		if (inactive)
		{
			iconColor.w *= 0.4f;
		}
		ImGui::TextColored(iconColor, "%s", badge.icon);
		if (inactive)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::WithAlpha(chrome::kMuted, 0.62f));
		}

		if (m_renaming == e)
		{
			ImGui::SameLine();
			if (m_renameFocusPending)
			{
				ImGui::SetKeyboardFocusHere();
				m_renameFocusPending = false;
			}
			ImGui::SetNextItemWidth(-60.0f);
			const bool entered = ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
			const bool deactivated = ImGui::IsItemDeactivated();
			if (entered || deactivated)
			{
				if ((entered || !ImGui::IsKeyPressed(ImGuiKey_Escape)) && m_renameBuf[0] != '\0')
				{
					world.EmplaceOrReplace<NameComponent>(e, NameComponent{.name = m_renameBuf});
					m_dirty = true;
				}
				m_renaming = {};
			}
		}
		else
		{
			ImGui::SameLine();
			const std::string name = EntityDisplayName(world, e);
			if (searching && !needle.empty())
			{
				const std::string lower = ToLower(name);
				const size_t pos = lower.find(needle);
				if (pos != std::string::npos)
				{
					if (pos > 0)
					{
						ImGui::TextUnformatted(name.substr(0, pos).c_str());
						ImGui::SameLine(0, 0);
					}
					ImGui::PushStyleColor(ImGuiCol_Text, chrome::kAccentHi);
					ImGui::TextUnformatted(name.substr(pos, needle.length()).c_str());
					ImGui::PopStyleColor();
					ImGui::SameLine(0, 0);
					if (pos + needle.length() < name.length())
					{
						ImGui::TextUnformatted(name.substr(pos + needle.length()).c_str());
					}
				}
				else
				{
					ImGui::TextUnformatted(name.c_str());
				}
			}
			else
			{
				ImGui::TextUnformatted(name.c_str());
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("#%u", e.id);

		if (inactive)
		{
			ImGui::PopStyleColor();
		}
	}

	void HierarchyPanel::FlattenNode(World& world, Entity e, int depth, std::uint64_t openMask)
	{
		m_flatTree.push_back({e, depth, openMask});
		if (m_expandedNodes.contains(e.id))
		{
			if (const auto* h = world.TryGet<HierarchyComponent>(e))
			{
				for (std::size_t i = 0; i < h->children.size(); ++i)
				{
					std::uint64_t childOpenMask = openMask;
					if (depth < 63)
					{
						const std::uint64_t siblingBit = 1ull << static_cast<unsigned>(depth);
						if (i + 1 < h->children.size())
						{
							childOpenMask |= siblingBit;
						}
						else
						{
							childOpenMask &= ~siblingBit;
						}
					}
					FlattenNode(world, h->children[i], depth + 1, childOpenMask);
				}
			}
		}
	}

	void HierarchyPanel::DrawTreeGuideLines(ImDrawList* drawList, const FlatTreeEntry& entry, const ImVec2& rowMin, const ImVec2& rowMax) const
	{
		const float arrowCenter = ImGui::GetFontSize() * 0.5f;
		const float indentSp = ImGui::GetStyle().IndentSpacing + 6.0f;
		const ImU32 lineCol = chrome::U32(chrome::WithAlpha(chrome::kMuted, 0.55f));
		const float cx = (rowMin.y + rowMax.y) * 0.5f;

		for (int d = 0; d + 1 < entry.depth; ++d)
		{
			if ((entry.openMask & (1ull << d)) != 0u)
			{
				const float vx = rowMin.x + static_cast<float>(d) * indentSp + arrowCenter;
				drawList->AddLine(ImVec2(vx, rowMin.y), ImVec2(vx, rowMax.y), lineCol, 1.25f);
			}
		}
		if (entry.depth > 0)
		{
			const float vx = rowMin.x + static_cast<float>(entry.depth - 1) * indentSp + arrowCenter;
			const float hxEnd = rowMin.x + static_cast<float>(entry.depth) * indentSp;
			const bool parentHasMoreSiblings = (entry.openMask & (1ull << static_cast<unsigned>(entry.depth - 1))) != 0;
			drawList->AddLine(ImVec2(vx, rowMin.y), ImVec2(vx, parentHasMoreSiblings ? rowMax.y : cx), lineCol, 1.25f);
			drawList->AddLine(ImVec2(vx, cx), ImVec2(hxEnd, cx), lineCol, 1.25f);
		}
	}

	void HierarchyPanel::UpdateKeyboardFocusScopeFromMouse(const ImVec2& sceneListMin, const ImVec2& sceneListMax)
	{
		if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseClicked(ImGuiMouseButton_Right))
		{
			return;
		}

		m_keyboardFocusScope = ImGui::IsMouseHoveringRect(sceneListMin, sceneListMax, false) ? KeyboardFocusScope::SceneList : KeyboardFocusScope::None;
	}

	bool HierarchyPanel::SceneListOwnsKeyboard() const noexcept
	{
		return m_keyboardFocusScope == KeyboardFocusScope::SceneList;
	}

	void HierarchyPanel::DrawRowUtilityToggles(World& world, Entity e)
	{
		const float rowEndX = ImGui::GetContentRegionMax().x + ImGui::GetCursorPosX() - ImGui::GetContentRegionAvail().x;
		const ImGuiStyle& style = ImGui::GetStyle();

		const bool selfDisabled = world.Has<DisabledComponent>(e);
		const bool inactive = ecs::HasDisabledAncestor(world, e);
		const bool hidden = world.Has<HiddenTag>(e);
		const bool notPickable = world.Has<NotPickableTag>(e);

		struct Toggle
		{
			const char* icon;
			ImVec4 color;
			const char* tip;
			int kind;
		};

		const Toggle toggles[3] = {
		        {ICON_FA_POWER_OFF,
		                selfDisabled ? ImVec4(0.86f, 0.45f, 0.40f, 0.95f) : (inactive ? chrome::WithAlpha(chrome::kFaint, 0.55f) : chrome::WithAlpha(chrome::kMuted, 0.9f)),
		                selfDisabled ? "Enable entity" : "Disable entity (and children)",
		                0},
		        {ICON_FA_EYE, hidden ? chrome::WithAlpha(chrome::kFaint, 0.45f) : chrome::WithAlpha(chrome::kMuted, 0.9f), hidden ? "Show in Scene View" : "Hide in Scene View", 1},
		        {notPickable ? ICON_FA_LOCK : ICON_FA_UNLOCK, notPickable ? chrome::WithAlpha(chrome::kFaint, 0.45f) : chrome::WithAlpha(chrome::kMuted, 0.9f), notPickable ? "Allow picking in Scene View" : "Disable picking in Scene View", 2},
		};

		float totalWidth = style.ItemSpacing.x * 2.0f;
		for (const Toggle& t: toggles)
		{
			totalWidth += ImGui::CalcTextSize(t.icon).x + style.FramePadding.x * 2.0f;
		}

		ImGui::SameLine();
		ImGui::SetCursorPosX(rowEndX - totalWidth);
		for (int i = 0; i < 3; ++i)
		{
			if (i > 0)
			{
				ImGui::SameLine();
			}
			ImGui::PushStyleColor(ImGuiCol_Text, toggles[i].color);
			const bool clicked = ImGui::SmallButton(toggles[i].icon);
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", toggles[i].tip);
			}
			if (!clicked)
			{
				continue;
			}
			switch (toggles[i].kind)
			{
				case 0:
					if (selfDisabled)
					{
						world.Remove<DisabledComponent>(e);
					}
					else
					{
						world.EmplaceOrReplace<DisabledComponent>(e);
					}
					break;
				case 1:
					if (hidden)
					{
						world.Remove<HiddenTag>(e);
					}
					else
					{
						world.Emplace<HiddenTag>(e);
					}
					break;
				default:
					if (notPickable)
					{
						world.Remove<NotPickableTag>(e);
					}
					else
					{
						world.Emplace<NotPickableTag>(e);
					}
					break;
			}
			m_dirty = true;
		}
	}

	void HierarchyPanel::DrawNode(app::LayerContext& context, World& world, SceneSelection& selection, Entity e, int depth, int flatTreeIndex, bool searching, std::string_view needle)
	{
		const auto* h = world.TryGet<HierarchyComponent>(e);
		const bool hasKids = (h != nullptr) && !h->children.empty();
		const bool isExpanded = hasKids && m_expandedNodes.contains(e.id);
		const int rowIndex = flatTreeIndex;

		ImGui::PushID(static_cast<int>(e.id));

		DrawRowBackdrop(selection, e, rowIndex);

		const float indentSp = ImGui::GetStyle().IndentSpacing + 6.0f;
		const float depthOffset = static_cast<float>(depth) * indentSp;
		const float arrowSlot = ImGui::GetFontSize();
		const ImVec2 rowStart = ImGui::GetCursorScreenPos();

		const float utilityReserve = ImGui::GetFrameHeight() * 5.0f;
		const float fullRowWidth = ImGui::GetContentRegionAvail().x;
		const float rowWidth = std::max(1.0f, fullRowWidth - utilityReserve);
		ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
		ImGui::InvisibleButton("##row", ImVec2(rowWidth, ImGui::GetFrameHeight()), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		ImGui::PopItemFlag();
		const ImVec2 rowMin = ImGui::GetItemRectMin();
		const ImVec2 rowMax = ImGui::GetItemRectMax();
		const ImVec2 visualRowMax(rowMin.x + fullRowWidth, rowMax.y);
		const bool visualRowHovered = ImGui::IsMouseHoveringRect(rowMin, visualRowMax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		const ImVec2 afterSelectable = ImGui::GetCursorScreenPos();

		if (m_scrollToEntity == e)
		{
			ImGui::SetScrollHereY(0.5f);
			m_scrollToEntity = {};
		}

		if (visualRowHovered && !selection.Contains(e) && !ImGui::IsDragDropActive())
		{
			ImGui::GetWindowDrawList()->AddRectFilled(rowMin, visualRowMax, chrome::U32(chrome::kHoverBg));
		}

		const ImVec2 arrowMin(rowStart.x + depthOffset, rowMin.y);
		const ImVec2 arrowMax(arrowMin.x + arrowSlot, rowMax.y);
		const bool arrowHovered = hasKids && ImGui::IsMouseHoveringRect(arrowMin, arrowMax);
		if (arrowHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsDragDropActive())
		{
			if (isExpanded)
			{
				m_expandedNodes.erase(e.id);
			}
			else
			{
				m_expandedNodes.insert(e.id);
			}
		}

		if (!arrowHovered || ImGui::IsDragDropActive())
		{
			HandleRowClick(selection, e);
			const float gapHalf = ImGui::GetStyle().ItemSpacing.y * 0.5f;
			HandleRowDragDrop(context, world, selection, e, rowMin.y - gapHalf, rowMax.y + gapHalf, visualRowMax.x);
		}
		const bool destroyed = DrawRowContextMenu(world, selection, e);

		const ImVec2 contentStart(rowStart.x + depthOffset + arrowSlot + ImGui::GetStyle().ItemInnerSpacing.x, rowStart.y);
		ImGui::SetCursorScreenPos(contentStart);
		if (!destroyed)
		{
			DrawRowContent(world, e, searching, needle, false);
		}

		if (!m_flatTree.empty())
		{
			const std::size_t idx = static_cast<std::size_t>(flatTreeIndex);
			if (idx < m_flatTree.size())
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImGuiWindow* window = ImGui::GetCurrentWindowRead();
				drawList->PushClipRect(window->ClipRect.Min, window->ClipRect.Max, true);
				DrawTreeGuideLines(drawList, m_flatTree[idx], rowStart, rowMax);
				drawList->PopClipRect();
			}
		}

		if (hasKids)
		{
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			const ImU32 arrowColor = arrowHovered ? ImGui::GetColorU32(ImGuiCol_Text) : chrome::U32(chrome::WithAlpha(chrome::kText, 0.82f));
			const float midY = (arrowMin.y + arrowMax.y) * 0.5f;
			const float size = std::min(arrowSlot, rowMax.y - rowMin.y) * 0.58f;
			const float left = arrowMin.x + (arrowSlot - size) * 0.5f;
			const float top = midY - size * 0.5f;
			if (isExpanded)
			{
				drawList->AddTriangleFilled(ImVec2(left, top + size * 0.28f), ImVec2(left + size, top + size * 0.28f), ImVec2(left + size * 0.5f, top + size * 0.82f), arrowColor);
			}
			else
			{
				drawList->AddTriangleFilled(ImVec2(left + size * 0.30f, top), ImVec2(left + size * 0.30f, top + size), ImVec2(left + size * 0.82f, top + size * 0.5f), arrowColor);
			}
		}

		if (!destroyed)
		{
			DrawRowUtilityToggles(world, e);
		}

		ImGui::SetCursorScreenPos(afterSelectable);
		ImGui::Dummy(ImVec2(0.0f, 0.0f));
		ImGui::PopID();
	}

	void HierarchyPanel::RequestSaveAsPopup()
	{
		m_requestSaveAsPopup = true;
		SetVisible(true);
	}

	void HierarchyPanel::RequestOpenPopup()
	{
		m_requestOpenPopup = true;
		SetVisible(true);
	}

	// Centered "Open Scene" dialog: searchable scene list with kind badges,
	// entity counts, and a startup-scene toggle. Double-click or Enter opens.
	void HierarchyPanel::DrawOpenSceneModal(app::LayerContext& context, World& world, SceneSelection& selection)
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_Appearing);
		ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 320.0f), ImVec2(FLT_MAX, FLT_MAX));
		bool open = true;
		if (!ImGui::BeginPopupModal("Open Scene", &open, ImGuiWindowFlags_NoCollapse))
		{
			return;
		}

		if (m_sceneListDirty)
		{
			m_sceneListDirty = false;
			m_sceneEntries.clear();
			for (const std::string& name: app::scene::ListSceneFiles())
			{
				SceneListEntry entry;
				entry.name = name;
				if (const auto desc = app::scene::ReadSceneFile(name))
				{
					entry.kindLabel = desc->kind == SceneKind::Scene2D ? "2D" : desc->kind == SceneKind::Mixed ? "Mixed" : "3D";
					entry.entityCount = desc->entities.size();
				}
				m_sceneEntries.push_back(std::move(entry));
			}
		}

		auto* settingsService = context.TryGet<aether::SettingsService>();
		auto* scenes = context.TryGet<aether::SceneSubsystem>();
		const std::string currentScene = scenes != nullptr ? scenes->GetCurrentScene() : std::string{};

		const auto loadScene = [&](const std::string& name)
		{
			if (app::scene::LoadSceneFile(name, world, app::scene::MakeApplySceneDeps(context.services)))
			{
				selection.Clear();
				if (scenes != nullptr)
				{
					scenes->SetCurrentScene(name);
				}
			}
			ImGui::CloseCurrentPopup();
		};

		// ── Search row ────────────────────────────────────────────────────────
		ImGui::TextColored(chrome::kAccentHi, ICON_FA_MAGNIFYING_GLASS);
		ImGui::SameLine();
		if (m_sceneSearchFocusPending)
		{
			ImGui::SetKeyboardFocusHere();
			m_sceneSearchFocusPending = false;
		}
		ImGui::SetNextItemWidth(-64.0f);
		ImGui::InputTextWithHint("##sceneSearch", "Search scenes...", m_sceneSearch, sizeof(m_sceneSearch));
		const bool searchEntered = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
		ImGui::SameLine();
		if (chrome::GhostIconButton(ICON_FA_ROTATE, "##refreshScenes", ImVec2(28.0f, 0.0f)))
		{
			m_sceneListDirty = true;
		}
		ImGui::SetItemTooltip("Rescan the scenes folder");

		// ── Scene list ────────────────────────────────────────────────────────
		const float footerHeight = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, chrome::WithAlpha(chrome::kPanel, 0.6f));
		ImGui::BeginChild("##sceneRows", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);
		const std::string_view needle{m_sceneSearch};
		std::size_t shown = 0;
		std::string firstVisible;
		for (const SceneListEntry& entry: m_sceneEntries)
		{
			if (!needle.empty() && std::search(entry.name.begin(), entry.name.end(), needle.begin(), needle.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) == entry.name.end())
			{
				continue;
			}
			++shown;
			if (firstVisible.empty())
			{
				firstVisible = entry.name;
			}
			ImGui::PushID(entry.name.c_str());
			const bool isCurrent = entry.name == currentScene;
			const bool isStartup = settingsService != nullptr && settingsService->Get().app.startupScene == entry.name;
			const bool isSelected = m_openSceneSelected == entry.name;

			const float rowStart = ImGui::GetCursorPosX();
			if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, ImGui::GetFrameHeight() + 6.0f)))
			{
				m_openSceneSelected = entry.name;
				if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
				{
					loadScene(entry.name);
				}
			}
			const ImVec2 rowMin = ImGui::GetItemRectMin();
			const ImVec2 rowMax = ImGui::GetItemRectMax();
			const float rowMidY = (rowMin.y + rowMax.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f;

			// Kind badge + name + metadata, drawn over the selectable.
			ImGui::SetCursorScreenPos(ImVec2(rowMin.x + 8.0f, rowMidY));
			ImGui::TextColored(chrome::WithAlpha(chrome::kAccentHi, 0.9f), "%s", entry.kindLabel.empty() ? "?" : entry.kindLabel.c_str());
			ImGui::SameLine(rowStart + 52.0f);
			ImGui::TextUnformatted(entry.name.c_str());
			if (isCurrent)
			{
				ImGui::SameLine();
				ImGui::TextColored(chrome::kMuted, "(current)");
			}

			// Right side: entity count + startup star.
			char meta[48]{};
			std::snprintf(meta, sizeof(meta), "%zu %s", entry.entityCount, entry.entityCount == 1 ? "entity" : "entities");
			const float starWidth = 26.0f;
			const float metaWidth = ImGui::CalcTextSize(meta).x;
			ImGui::SameLine(ImGui::GetContentRegionMax().x - metaWidth - starWidth - 16.0f);
			ImGui::TextColored(chrome::kMuted, "%s", meta);
			ImGui::SameLine(ImGui::GetContentRegionMax().x - starWidth - 4.0f);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
			if (isStartup)
			{
				ImGui::TextColored(chrome::kAccentHi, ICON_FA_STAR);
				ImGui::SetItemTooltip("Startup scene (loaded when the game boots)");
			}
			else if (settingsService != nullptr)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, chrome::WithAlpha(chrome::kMuted, 0.35f));
				const bool setStartup = ImGui::SmallButton(ICON_FA_STAR "##startup");
				ImGui::PopStyleColor();
				if (setStartup)
				{
					settingsService->Values().app.startupScene = entry.name;
					settingsService->ApplyField("app.startupScene");
					settingsService->Save();
				}
				ImGui::SetItemTooltip("Make this the startup scene");
			}
			ImGui::PopID();
		}
		if (m_sceneEntries.empty())
		{
			ImGui::TextColored(chrome::kMuted, "No scenes in %s", app::scene::ScenesDirectory().c_str());
		}
		else if (shown == 0)
		{
			ImGui::TextColored(chrome::kMuted, "No scene matches '%s'", m_sceneSearch);
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();

		// ── Footer ────────────────────────────────────────────────────────────
		const bool canOpen = !m_openSceneSelected.empty() || (shown == 1 && !firstVisible.empty());
		const std::string openTarget = !m_openSceneSelected.empty() ? m_openSceneSelected : firstVisible;
		ImGui::Spacing();
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 196.0f);
		if (chrome::OutlineButton("Cancel", ImVec2(90.0f, 0.0f)))
		{
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(!canOpen);
		if (chrome::PrimaryButton(ICON_FA_FOLDER_OPEN "  Open", ImVec2(98.0f, 0.0f)) || (canOpen && (searchEntered || ImGui::IsKeyPressed(ImGuiKey_Enter, false))))
		{
			loadScene(openTarget);
		}
		ImGui::EndDisabled();

		ImGui::EndPopup();
	}

	void HierarchyPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();
		World& world = context.Get<World>();
		auto& selection = context.Get<SceneSelection>();
		auto& reg = world.GetRegistry();

		m_rowsPrev = std::move(m_rowsCur);
		m_rowsCur.clear();

		// drag) so a stale press can never collapse a later selection.
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m_pendingCollapse.IsValid())
		{
			m_pendingCollapse = {};
		}

		const double now = ImGui::GetTime();
		{
			m_knownIdsScratch.clear();
			m_knownIdsScratch.reserve(m_knownIds.size());
			for (const auto handle: reg.storage<entt::entity>())
			{
				if (!reg.valid(handle))
				{
					continue;
				}
				const Entity e = World::FromEntt(handle);
				if (!e.IsValid())
				{
					continue;
				}
				m_knownIdsScratch.insert(e.id);
				if (m_knownSeeded && !m_knownIds.contains(e.id))
				{
					m_spawnFlash[e.id] = now;
				}
			}
			m_knownIds.swap(m_knownIdsScratch);
			m_knownSeeded = true;

			for (auto it = m_spawnFlash.begin(); it != m_spawnFlash.end();)
			{
				it = (now - it->second > 1.0) ? m_spawnFlash.erase(it) : std::next(it);
			}
		}
		if (selection.ChangeSerial() != m_seenSelectionSerial)
		{
			m_seenSelectionSerial = selection.ChangeSerial();
			m_pulseStart = now;
		}

		ImGui::Begin("Scene", VisiblePtr());
		{
			if (m_requestSaveAsPopup)
			{
				m_requestSaveAsPopup = false;
				ImGui::OpenPopup("Save Scene As");
			}
			if (m_requestOpenPopup)
			{
				m_requestOpenPopup = false;
				m_sceneListDirty = true;
				m_sceneSearch[0] = '\0';
				m_sceneSearchFocusPending = true;
				m_openSceneSelected.clear();
				ImGui::OpenPopup("Open Scene");
			}

			std::size_t count = 0;
			for (const auto handle: reg.storage<entt::entity>())
			{
				if (reg.valid(handle) && World::FromEntt(handle).IsValid())
				{
					++count;
				}
			}
			{
				const auto* scenes = context.TryGet<aether::SceneSubsystem>();
				const char* sceneName = (scenes != nullptr && !scenes->GetCurrentScene().empty()) ? scenes->GetCurrentScene().c_str() : "Untitled";
				char countText[32]{};
				std::snprintf(countText, sizeof(countText), "%zu ENTITIES", count);
				chrome::PanelHeader(sceneName, countText);
			}

			const float tbBtnH = ImGui::GetFrameHeight();
			const auto tbIconW = [&](const char* icon)
			{
				return std::max(tbBtnH, ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2.0f);
			};

			if (chrome::GhostIconButton(ICON_FA_PLUS, "##addEntity", ImVec2(tbIconW(ICON_FA_PLUS), tbBtnH), chrome::kAccentHi))
			{
				ImGui::OpenPopup("CreateEntity");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Create entity");
			}
			if (ImGui::BeginPopup("CreateEntity"))
			{
				// Unity-style: every create entry is available in every scene -
				// 2D and 3D content freely mix, so nothing is hidden by kind.
				if (ImGui::MenuItem(ICON_FA_CIRCLE "  Empty entity"))
				{
					const Entity e = world.Create();
					world.Emplace<NameComponent>(e, NameComponent{.name = "Entity"});
					selection.Select(e);
				}
				ImGui::Separator();
				if (ImGui::BeginMenu(ICON_FA_IMAGE "  2D"))
				{
					if (ImGui::MenuItem(ICON_FA_IMAGE "  Sprite"))
					{
						(void) CreateSpriteEntity(context, world, selection, false);
					}
					if (ImGui::MenuItem(ICON_FA_FILM "  Animated Sprite"))
					{
						(void) CreateSpriteEntity(context, world, selection, true);
					}
					if (ImGui::MenuItem(ICON_FA_IMAGE "  Tile Map"))
					{
						const Entity entity = world.Create();
						world.Emplace<NameComponent>(entity, NameComponent{.name = "Tile Map"});
						world.Emplace<TransformComponent>(entity);
						world.Emplace<TileMapComponent>(entity);
						selection.Select(entity);
					}
					ImGui::Separator();
					if (ImGui::MenuItem(ICON_FA_VIDEO "  Orthographic Camera"))
					{
						CameraComponent camera{};
						camera.projection = CameraProjection::Orthographic;
						camera.orthographicHeight = 10.0f;
						const Entity entity = ecs::CreateCameraEntity(world, {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, -1.0f}, camera, "2D Camera");
						if (!ecs::GetMainCameraEntity(world).IsValid())
						{
							ecs::SetMainCameraEntity(world, entity);
						}
						selection.Select(entity);
					}
					ImGui::EndMenu();
				}
				{
					ImGui::Separator();
					if (ImGui::MenuItem(ICON_FA_CUBE "  Cube"))
					{
						CreatePrimitive(context, world, selection, PrimitiveMesh::Cube, "Cube", "cube");
					}
					if (ImGui::MenuItem(ICON_FA_CIRCLE "  Sphere"))
					{
						CreatePrimitive(context, world, selection, PrimitiveMesh::Sphere, "Sphere", "sphere");
					}
					if (ImGui::MenuItem(ICON_FA_IMAGE "  Plane"))
					{
						CreatePrimitive(context, world, selection, PrimitiveMesh::Plane, "Plane", "plane");
					}
					if (ImGui::MenuItem(ICON_FA_IMAGE "  Quad"))
					{
						CreatePrimitive(context, world, selection, PrimitiveMesh::Quad, "Quad", "quad");
					}
					if (ImGui::MenuItem(ICON_FA_PLAY "  Triangle"))
					{
						CreatePrimitive(context, world, selection, PrimitiveMesh::Triangle, "Triangle", "triangle");
					}
				}
				{
					ImGui::Separator();
					if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light"))
					{
						selection.Select(ecs::CreatePointLightEntity(world, {0.0f, 5.0f, 0.0f}, PointLightComponent{}));
					}
					if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Spot Light"))
					{
						selection.Select(ecs::CreateSpotLightEntity(world, {0.0f, 8.0f, 0.0f}, {0.0f, -0.85f, -0.5f}, SpotLightComponent{}));
					}
				}
				{
					ImGui::Separator();
					if (ImGui::MenuItem(ICON_FA_VIDEO "  Camera"))
					{
						const Entity cam = ecs::CreateCameraEntity(world, {0.0f, 3.0f, 8.0f}, {0.0f, -0.35f, -1.0f}, CameraComponent{});
						if (!ecs::GetMainCameraEntity(world).IsValid())
						{
							ecs::SetMainCameraEntity(world, cam);
						}
						selection.Select(cam);
					}
				}
				ImGui::Separator();
				if (ImGui::BeginMenu(ICON_FA_IMAGE "  UI"))
				{
					if (ImGui::MenuItem("Canvas"))
					{
						selection.Select(ui::CreateCanvasEntity(world));
					}
					if (ImGui::MenuItem("Image"))
					{
						selection.Select(ui::CreateImageEntity(world, FindOrCreateCanvas(world)));
					}
					if (ImGui::MenuItem("Text"))
					{
						selection.Select(ui::CreateTextEntity(world, FindOrCreateCanvas(world)));
					}
					ImGui::EndMenu();
				}
				ImGui::EndPopup();
			}

			{
				const ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
				bool saveSceneOpen = true;
				if (ImGui::BeginPopupModal("Save Scene As", &saveSceneOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
				{
					if (ImGui::IsWindowAppearing())
					{
						m_saveExistingNames = app::scene::ListSceneFiles();
					}
					ImGui::TextColored(chrome::kAccentHi, ICON_FA_FLOPPY_DISK);
					ImGui::SameLine();
					if (ImGui::IsWindowAppearing())
					{
						ImGui::SetKeyboardFocusHere();
					}
					ImGui::SetNextItemWidth(-FLT_MIN);
					const bool entered = ImGui::InputTextWithHint("##sceneName", "Scene name...", m_sceneNameBuf, sizeof(m_sceneNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
					ImGui::TextColored(chrome::kMuted, ICON_FA_FOLDER_OPEN "  %s", app::scene::ScenesDirectory().c_str());
					const bool exists = m_sceneNameBuf[0] != '\0' && std::ranges::find(m_saveExistingNames, m_sceneNameBuf) != m_saveExistingNames.end();
					if (exists)
					{
						ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION "  Overwrites the existing '%s' scene", m_sceneNameBuf);
					}
					ImGui::Spacing();
					ImGui::SameLine(ImGui::GetContentRegionMax().x - 196.0f);
					if (chrome::OutlineButton("Cancel", ImVec2(90.0f, 0.0f)))
					{
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					ImGui::BeginDisabled(m_sceneNameBuf[0] == '\0');
					if (chrome::PrimaryButton(exists ? ICON_FA_FLOPPY_DISK "  Overwrite" : ICON_FA_FLOPPY_DISK "  Save", ImVec2(98.0f, 0.0f)) || (entered && m_sceneNameBuf[0] != '\0'))
					{
						if (auto* assets = context.TryGet<AssetManager>())
						{
							const auto captured = app::scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
							app::scene::SaveSceneFile(m_sceneNameBuf, captured);
							m_sceneListDirty = true;
							if (auto* scenes = context.TryGet<aether::SceneSubsystem>())
							{
								scenes->SetCurrentScene(m_sceneNameBuf);
							}
						}
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndDisabled();
					ImGui::EndPopup();
				}
			}
			DrawOpenSceneModal(context, world, selection);

			if (m_openPrefabSave)
			{
				ImGui::OpenPopup("Save Prefab");
				m_openPrefabSave = false;
			}
			{
				const ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
				ImGui::SetNextWindowSize(ImVec2(440.0f, 0.0f), ImGuiCond_Appearing);
				bool savePrefabOpen = true;
				if (ImGui::BeginPopupModal("Save Prefab", &savePrefabOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
				{
					auto& reg2 = world.GetRegistry();
					if (!m_prefabSaveTarget.IsValid() || !reg2.valid(World::ToEntt(m_prefabSaveTarget)))
					{
						ImGui::CloseCurrentPopup();
					}
					else
					{
						if (ImGui::IsWindowAppearing())
						{
							m_saveExistingNames = app::scene::ListPrefabFiles();
						}
						ImGui::TextColored(chrome::kMuted, ICON_FA_BOX_OPEN "  Prefab of '%s' (includes children)", EntityDisplayName(world, m_prefabSaveTarget));
						ImGui::Spacing();
						ImGui::TextColored(chrome::kAccentHi, ICON_FA_FLOPPY_DISK);
						ImGui::SameLine();
						if (ImGui::IsWindowAppearing())
						{
							ImGui::SetKeyboardFocusHere();
						}
						ImGui::SetNextItemWidth(-FLT_MIN);
						const bool entered = ImGui::InputTextWithHint("##prefabName", "Prefab name...", m_prefabNameBuf, sizeof(m_prefabNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
						ImGui::TextColored(chrome::kMuted, ICON_FA_FOLDER_OPEN "  %s", app::scene::PrefabsDirectory().c_str());
						const bool exists = m_prefabNameBuf[0] != '\0' && std::ranges::find(m_saveExistingNames, m_prefabNameBuf) != m_saveExistingNames.end();
						if (exists)
						{
							ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION "  Overwrites the existing '%s' prefab", m_prefabNameBuf);
						}
						ImGui::Spacing();
						ImGui::SameLine(ImGui::GetContentRegionMax().x - 196.0f);
						if (chrome::OutlineButton("Cancel", ImVec2(90.0f, 0.0f)))
						{
							ImGui::CloseCurrentPopup();
						}
						ImGui::SameLine();
						ImGui::BeginDisabled(m_prefabNameBuf[0] == '\0');
						if (chrome::PrimaryButton(exists ? ICON_FA_FLOPPY_DISK "  Overwrite" : ICON_FA_FLOPPY_DISK "  Save", ImVec2(98.0f, 0.0f)) || (entered && m_prefabNameBuf[0] != '\0'))
						{
							if (auto* assets = context.TryGet<AssetManager>())
							{
								app::scene::SavePrefabFile(m_prefabNameBuf, app::scene::CapturePrefab(world, m_prefabSaveTarget, assets->GetMaterialRegistry(), assets->GetTextureRegistry()));
							}
							ImGui::CloseCurrentPopup();
						}
						ImGui::EndDisabled();
					}
					ImGui::EndPopup();
				}
			}

			ImGui::SameLine();
			ImGui::TextDisabled(ICON_FA_MAGNIFYING_GLASS);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##search", "Search name or #id...", m_search, sizeof(m_search));

			FilterChip(ICON_FA_CUBE, "Meshes", ImVec4(0.62f, 0.88f, 0.62f, 1.0f), m_filterMesh);
			ImGui::SameLine();
			FilterChip(ICON_FA_PERSON_RUNNING, "Skinned", ImVec4(0.55f, 0.75f, 1.00f, 1.0f), m_filterSkinned);
			ImGui::SameLine();
			FilterChip(ICON_FA_WEIGHT_HANGING, "Physics", ImVec4(1.00f, 0.72f, 0.35f, 1.0f), m_filterPhysics);
			ImGui::SameLine();
			FilterChip(ICON_FA_WAND_MAGIC_SPARKLES, "Effects", ImVec4(0.80f, 0.55f, 1.00f, 1.0f), m_filterEffect);

			if (!m_expandedPathsLoaded)
			{
				SyncExpandedFromPaths(world);
			}

			DrawBreadcrumbTrail(world, selection);

			const bool anyChip = m_filterMesh || m_filterSkinned || m_filterPhysics || m_filterEffect;
			const bool searching = m_search[0] != '\0';
			const bool filtering = searching || anyChip;

			auto passesChips = [&](Entity e)
			{
				if (!anyChip)
				{
					return true;
				}
				return (m_filterMesh && world.Has<MeshComponent>(e)) || (m_filterSkinned && world.Has<SkinnedMeshComponent>(e)) || (m_filterPhysics && world.Has<RigidBodyComponent>(e)) || (m_filterEffect && world.Has<EffectParamsComponent>(e));
			};

			ImGui::BeginChild("SceneList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);
			const ImVec2 sceneListMin = ImGui::GetWindowPos();
			const ImVec2 sceneListMax(sceneListMin.x + ImGui::GetWindowSize().x, sceneListMin.y + ImGui::GetWindowSize().y);
			UpdateKeyboardFocusScopeFromMouse(sceneListMin, sceneListMax);
			if (filtering)
			{
				const std::string needle = ToLower(m_search);
				m_filteredRowsScratch.clear();
				m_filteredRowsScratch.reserve(count);
				for (const auto handle: reg.storage<entt::entity>())
				{
					if (!reg.valid(handle))
					{
						continue;
					}
					const Entity e = World::FromEntt(handle);
					if (!e.IsValid() || !passesChips(e))
					{
						continue;
					}
					if (searching)
					{
						char idText[16]{};
						std::snprintf(idText, sizeof(idText), "#%u", e.id);
						if (!ContainsCaseInsensitive(EntityDisplayName(world, e), needle) && !std::string_view(idText).contains(needle))
						{
							continue;
						}
					}
					m_filteredRowsScratch.push_back(e);
				}

				m_rowsCur = m_filteredRowsScratch;
				int scrollToIndex = -1;
				if (m_scrollToEntity.IsValid())
				{
					if (const auto it = std::find(m_filteredRowsScratch.begin(), m_filteredRowsScratch.end(), m_scrollToEntity); it != m_filteredRowsScratch.end())
					{
						scrollToIndex = static_cast<int>(std::distance(m_filteredRowsScratch.begin(), it));
					}
				}

				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_filteredRowsScratch.size()));
				if (scrollToIndex >= 0)
				{
					clipper.IncludeItemByIndex(scrollToIndex);
				}
				while (clipper.Step())
				{
					for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					{
						const Entity e = m_filteredRowsScratch[static_cast<std::size_t>(i)];
						ImGui::PushID(static_cast<int>(e.id));
						DrawRowBackdrop(selection, e, i);
						ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
						ImGui::Selectable("##row", selection.Contains(e), ImGuiSelectableFlags_SpanAllColumns);
						ImGui::PopItemFlag();
						const ImVec2 rowMin = ImGui::GetItemRectMin();
						const ImVec2 rowMax = ImGui::GetItemRectMax();
						if (m_scrollToEntity == e)
						{
							ImGui::SetScrollHereY(0.5f);
							m_scrollToEntity = {};
						}
						HandleRowClick(selection, e);
						HandleRowDragDrop(context, world, selection, e, rowMin.y, rowMax.y, rowMax.x);
						if (!DrawRowContextMenu(world, selection, e))
						{
							DrawRowContent(world, e, searching, needle);
						}
						ImGui::PopID();
					}
				}
				if (m_filteredRowsScratch.empty())
				{
					ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.4f));
					const char* msg = "No entities match";
					ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) * 0.5f);
					ImGui::TextDisabled("%s", msg);
				}
			}
			else if (count == 0)
			{
				ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.4f));
				const char* msg = "Scene is empty";
				ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) * 0.5f);
				ImGui::TextDisabled("%s", msg);
			}
			else
			{
				m_flatTree.clear();
				const auto& roots = world.Roots();
				for (std::size_t i = 0; i < roots.size(); ++i)
				{
					const Entity e = roots[i];
					if (e.IsValid() && reg.valid(World::ToEntt(e)))
					{
						const std::uint64_t openMask = (i + 1 < roots.size()) ? 1ull : 0ull;
						FlattenNode(world, e, 0, openMask);
					}
				}

				m_rowsCur.reserve(m_flatTree.size());
				for (const FlatTreeEntry& entry: m_flatTree)
				{
					m_rowsCur.push_back(entry.entity);
				}

				int scrollToIndex = -1;
				if (m_scrollToEntity.IsValid())
				{
					for (std::size_t i = 0; i < m_flatTree.size(); ++i)
					{
						if (m_flatTree[i].entity == m_scrollToEntity)
						{
							scrollToIndex = static_cast<int>(i);
							break;
						}
					}
				}

				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_flatTree.size()));
				if (scrollToIndex >= 0)
				{
					clipper.IncludeItemByIndex(scrollToIndex);
				}
				while (clipper.Step())
				{
					for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					{
						const FlatTreeEntry& entry = m_flatTree[static_cast<std::size_t>(i)];
						DrawNode(context, world, selection, entry.entity, entry.depth, i, false, {});
					}
				}

				const float remaining = std::min(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().y);
				if (remaining > 4.0f)
				{
					ImGui::InvisibleButton("##emptyDrop", ImVec2(ImGui::GetContentRegionAvail().x, remaining));
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(dragdrop::kEntityPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
						{
							const auto draggedId = *static_cast<const std::uint32_t*>(p->Data);
							m_pendingReparent = PendingReparent{Entity{draggedId}, Entity{}, DropZone::Inside};
						}
						ImGui::EndDragDropTarget();
					}
				}
			}
			ImGui::EndChild();

			if (!filtering && SceneListOwnsKeyboard())
			{
				HandleKeyboardNavigation(selection);
				HandleTypeToJump(world, selection);
			}

			// gizmo drag then Ctrl+D again must work without re-clicking the
			const bool panelKeys = !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl;
			auto* clipAssets = context.TryGet<AssetManager>();
			auto* undo = context.TryGet<UndoStack>();

			if (panelKeys && ImGui::IsKeyPressed(ImGuiKey_D, false))
			{
				m_pendingDuplicate = true;
			}
			const bool copyKey = m_pendingCopy || (panelKeys && ImGui::IsKeyPressed(ImGuiKey_C, false));
			const bool cutKey = m_pendingCut || (panelKeys && ImGui::IsKeyPressed(ImGuiKey_X, false));
			const bool pasteKey = m_pendingPaste || (panelKeys && ImGui::IsKeyPressed(ImGuiKey_V, false));
			m_pendingCopy = m_pendingCut = m_pendingPaste = false;

			if ((copyKey || cutKey) && clipAssets != nullptr && !selection.All().empty())
			{
				const std::vector<Entity> roots = CollectSelectionRoots(world, selection);
				if (!roots.empty())
				{
					ImGui::SetClipboardText(app::scene::WriteToml(app::scene::CaptureSubtrees(world, roots, clipAssets->GetMaterialRegistry(), clipAssets->GetTextureRegistry())).c_str());
					if (cutKey)
					{
						if (undo != nullptr)
						{
							undo->Push(world, context.services);
						}
						for (const Entity r: roots)
						{
							if (reg.valid(World::ToEntt(r)))
							{
								ecs::DestroyHierarchy(world, r);
							}
						}
						selection.Clear();
					}
				}
			}
			if (pasteKey)
			{
				if (const char* clip = ImGui::GetClipboardText(); clip != nullptr && clip[0] != '\0')
				{
					if (const auto parsed = app::scene::ParseToml(clip); parsed.has_value() && !parsed->entities.empty())
					{
						if (undo != nullptr)
						{
							undo->Push(world, context.services);
						}
						const auto created = app::scene::ApplyScene(*parsed, world, app::scene::MakeApplySceneDeps(context.services));
						bool first = true;
						for (std::size_t i = 0; i < parsed->entities.size() && i < created.size(); ++i)
						{
							if (parsed->entities[i].parentIndex >= 0)
							{
								continue;
							}
							if (auto* tc = world.TryGet<TransformComponent>(created[i]))
							{
								glm::mat4 m = tc->localToWorld;
								m[3].x += 1.0f;
								ecs::SetWorldTransform(world, created[i], m);
							}
							if (first)
							{
								selection.Select(created[i]);
								first = false;
							}
							else
							{
								selection.ToggleSelection(created[i]);
							}
						}
					}
				}
			}

			if (m_pendingDuplicate)
			{
				m_pendingDuplicate = false;
				auto* dupAssets = context.TryGet<AssetManager>();
				if (dupAssets != nullptr)
				{
					const std::vector<Entity> roots = CollectSelectionRoots(world, selection);
					if (!roots.empty() && undo != nullptr)
					{
						undo->Push(world, context.services);
					}
					bool first = true;
					for (const Entity root: roots)
					{
						const auto prefab = app::scene::CapturePrefab(world, root, dupAssets->GetMaterialRegistry(), dupAssets->GetTextureRegistry());
						glm::mat4 placed(1.0f);
						if (const auto* tc = world.TryGet<TransformComponent>(root))
						{
							placed = tc->localToWorld;
						}
						placed[3].x += 1.0f;
						const Entity copy = app::scene::InstantiatePrefab(prefab, world, app::scene::MakeApplySceneDeps(context.services), placed);
						if (copy.IsValid())
						{
							if (first)
							{
								selection.Select(copy);
								first = false;
							}
							else
							{
								selection.ToggleSelection(copy);
							}
						}
					}
				}
			}

			if (m_pendingReparent)
			{
				const PendingReparent pr = *m_pendingReparent;
				m_pendingReparent.reset();
				if (world.GetRegistry().valid(World::ToEntt(pr.child)))
				{
					std::vector<Entity> moved;
					if (selection.Contains(pr.child) && selection.All().size() > 1)
					{
						for (const Entity e: selection.All())
						{
							if (!world.GetRegistry().valid(World::ToEntt(e)))
							{
								continue;
							}
							bool ancestorSelected = false;
							const auto* h = world.TryGet<HierarchyComponent>(e);
							Entity cur = h ? h->parent : Entity{};
							while (cur.IsValid())
							{
								if (selection.Contains(cur))
								{
									ancestorSelected = true;
									break;
								}
								const auto* ch = world.TryGet<HierarchyComponent>(cur);
								cur = ch ? ch->parent : Entity{};
							}
							if (!ancestorSelected)
							{
								moved.push_back(e);
							}
						}
					}
					else
					{
						moved.push_back(pr.child);
					}
					int afterOffset = 0;
					bool changed = false;
					for (const Entity e: moved)
					{
						if (pr.zone == DropZone::Inside || !pr.target.IsValid())
						{
							changed = ecs::SetParent(world, e, pr.target) || changed;
							continue;
						}

						const auto* targetHierarchy = world.TryGet<HierarchyComponent>(pr.target);
						const Entity parent = targetHierarchy ? targetHierarchy->parent : Entity{};
						const std::vector<Entity>* siblings = nullptr;
						if (parent.IsValid())
						{
							const auto* parentHierarchy = world.TryGet<HierarchyComponent>(parent);
							if (!parentHierarchy)
							{
								continue;
							}
							siblings = &parentHierarchy->children;
						}
						else
						{
							siblings = &world.Roots();
						}

						const auto targetIt = std::find(siblings->begin(), siblings->end(), pr.target);
						if (targetIt == siblings->end())
						{
							continue;
						}

						int insertIndex = static_cast<int>(std::distance(siblings->begin(), targetIt));
						if (pr.zone == DropZone::After)
						{
							insertIndex += 1 + afterOffset;
						}

						const auto childIt = std::find(siblings->begin(), siblings->end(), e);
						if (childIt != siblings->end())
						{
							const int childIndex = static_cast<int>(std::distance(siblings->begin(), childIt));
							if (childIndex < insertIndex)
							{
								--insertIndex;
							}
						}

						if (ecs::InsertChildAt(world, e, parent, insertIndex))
						{
							changed = true;
							if (pr.zone == DropZone::After)
							{
								++afterOffset;
							}
						}
					}
					if (changed)
					{
						m_dirty = true;
					}
				}
			}

			if (!ImGui::GetIO().WantTextInput)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !selection.All().empty())
				{
					if (auto* undoStack = context.TryGet<UndoStack>())
					{
						undoStack->Push(world, context.services);
					}
					const std::vector<Entity> doomed = selection.All();
					for (const Entity e: doomed)
					{
						if (world.GetRegistry().valid(World::ToEntt(e)))
						{
							ecs::DestroyHierarchy(world, e);
						}
					}
					selection.Clear();
				}
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F2) && selection.Primary().IsValid())
				{
					BeginRename(world, selection.Primary());
				}
			}
		}
		ImGui::End();
	}

	std::string HierarchyPanel::ComputeEntityPath(const World& world, Entity e) const
	{
		std::vector<std::string> segments;
		Entity cur = e;
		while (cur.IsValid())
		{
			segments.emplace_back(EntityDisplayName(world, cur));
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			cur = h ? h->parent : Entity{};
		}
		std::string path;
		for (auto& segment: std::views::reverse(segments))
		{
			path += '/';
			path += segment;
		}
		return path;
	}

	namespace
	{
		Entity FindEntityByPath(const World& world, const std::vector<std::string>& segments)
		{
			if (segments.empty())
			{
				return {};
			}
			Entity cur{};
			for (const Entity e: world.Roots())
			{
				if (!e.IsValid() || !world.GetRegistry().valid(World::ToEntt(e)))
				{
					continue;
				}
				if (EntityDisplayName(world, e) == segments[0])
				{
					cur = e;
					break;
				}
			}
			if (!cur.IsValid())
			{
				return {};
			}
			for (std::size_t i = 1; i < segments.size(); ++i)
			{
				const auto* h = world.TryGet<HierarchyComponent>(cur);
				if (!h)
				{
					return {};
				}
				bool found = false;
				for (const Entity c: h->children)
				{
					if (EntityDisplayName(world, c) == segments[i])
					{
						cur = c;
						found = true;
						break;
					}
				}
				if (!found)
				{
					return {};
				}
			}
			return cur;
		}

		void SplitPath(std::string_view path, std::vector<std::string>& out)
		{
			out.clear();
			if (!path.empty() && path[0] == '/')
			{
				path = path.substr(1);
			}
			while (!path.empty())
			{
				const auto pos = path.find('/');
				if (pos == std::string_view::npos)
				{
					out.emplace_back(path);
					break;
				}
				out.emplace_back(path.substr(0, pos));
				path = path.substr(pos + 1);
			}
		}
	} // namespace

	void HierarchyPanel::SyncExpandedFromPaths(World& world)
	{
		if (m_expandedPaths.empty())
		{
			m_expandedPathsLoaded = true;
			return;
		}
		m_expandedNodes.clear();
		std::vector<std::string> segments;
		for (const auto& [path, expanded]: m_expandedPaths)
		{
			if (!expanded)
			{
				continue;
			}
			SplitPath(path, segments);
			const Entity e = FindEntityByPath(world, segments);
			if (e.IsValid())
			{
				m_expandedNodes.insert(e.id);
			}
		}
		m_expandedPathsLoaded = true;
	}

	void HierarchyPanel::DrawBreadcrumbTrail(const World& world, SceneSelection& selection)
	{
		const Entity primary = selection.Primary();
		if (!primary.IsValid())
		{
			ImGui::TextDisabled("No entity selected");
			return;
		}

		std::vector<Entity> chain;
		Entity cur = primary;
		while (cur.IsValid())
		{
			chain.push_back(cur);
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			cur = h ? h->parent : Entity{};
		}
		std::reverse(chain.begin(), chain.end());

		bool first = true;
		for (const Entity e: chain)
		{
			if (!first)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("/");
				ImGui::SameLine();
			}
			first = false;

			const bool isPrimary = e == primary;
			const char* label = EntityDisplayName(world, e);
			ImGui::PushID(static_cast<int>(e.id));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Text, isPrimary ? ImGui::GetStyle().Colors[ImGuiCol_Text] : chrome::WithAlpha(chrome::kMuted, 0.62f));
			if (ImGui::Selectable(label, false))
			{
				selection.Select(e);
			}
			ImGui::PopStyleColor(4);
			ImGui::PopStyleVar();
			ImGui::PopID();

			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("%s", isPrimary ? "Current selection" : "Click to select this ancestor");
			}
		}
	}

	void HierarchyPanel::HandleKeyboardNavigation(SceneSelection& selection)
	{
		const auto& rows = m_rowsCur;
		if (rows.empty() || ImGui::GetIO().WantTextInput)
		{
			return;
		}

		const auto selectedIt = std::find(rows.begin(), rows.end(), selection.Primary());
		if (selectedIt != rows.end())
		{
			m_focusedRowIndex = static_cast<int>(std::distance(rows.begin(), selectedIt));
		}
		else if (m_focusedRowIndex < 0 || m_focusedRowIndex >= static_cast<int>(rows.size()))
		{
			m_focusedRowIndex = 0;
		}

		int delta = 0;
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
		{
			delta = 1;
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
		{
			delta = -1;
		}

		if (delta != 0)
		{
			m_focusedRowIndex = std::clamp(m_focusedRowIndex + delta, 0, static_cast<int>(rows.size()) - 1);
			selection.Select(rows[m_focusedRowIndex]);
			m_scrollToEntity = rows[m_focusedRowIndex];
		}
	}

	void HierarchyPanel::HandleTypeToJump(const World& world, SceneSelection& selection)
	{
		const double now = ImGui::GetTime();
		constexpr double kTypeTimeout = 0.8;

		if (!m_typeJumpText.empty() && (now - m_typeJumpTime) > kTypeTimeout)
		{
			m_typeJumpText.clear();
		}

		for (int ch = ImGuiKey_A; ch <= ImGuiKey_Z; ++ch)
		{
			if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ch), false))
			{
				m_typeJumpText += static_cast<char>('a' + (ch - ImGuiKey_A));
				m_typeJumpTime = now;
				break;
			}
		}

		if (m_typeJumpText.empty())
		{
			return;
		}

		const auto& rows = m_rowsCur;
		for (auto row: rows)
		{
			const std::string name = EntityDisplayName(world, row);
			std::string lower;
			lower.reserve(name.size());
			for (const unsigned char c: name)
			{
				lower.push_back(static_cast<char>(std::tolower(c)));
			}
			if (lower.starts_with(m_typeJumpText))
			{
				selection.Select(row);
				m_scrollToEntity = row;
				break;
			}
		}
	}

	void HierarchyPanel::LoadSettings(TomlConfig& config, app::LayerContext& context)
	{
		(void) context;
		m_filterMesh = config.GetBool("debug.hierarchy.filterMesh", m_filterMesh);
		m_filterSkinned = config.GetBool("debug.hierarchy.filterSkinned", m_filterSkinned);
		m_filterPhysics = config.GetBool("debug.hierarchy.filterPhysics", m_filterPhysics);
		m_filterEffect = config.GetBool("debug.hierarchy.filterEffect", m_filterEffect);

		m_expandedPaths.clear();
		const int count = static_cast<int>(config.GetFloat("debug.hierarchy.expanded.count", 0.0f));
		for (int i = 0; i < count; ++i)
		{
			char key[96];
			std::snprintf(key, sizeof(key), "debug.hierarchy.expanded.%d", i);
			if (config.Has(key))
			{
				m_expandedPaths[std::string(key)] = true;
			}
		}
		m_expandedPathsLoaded = false;
	}

	void HierarchyPanel::SaveSettings(TomlConfig& config, app::LayerContext& context) const
	{
		(void) context;
		config.Set("debug.hierarchy.filterMesh", m_filterMesh);
		config.Set("debug.hierarchy.filterSkinned", m_filterSkinned);
		config.Set("debug.hierarchy.filterPhysics", m_filterPhysics);
		config.Set("debug.hierarchy.filterEffect", m_filterEffect);
	}
} // namespace aether::editor
