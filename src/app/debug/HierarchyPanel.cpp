#include "debug/HierarchyPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <entt/entt.hpp>
#include <imgui.h>
#include <imgui_internal.h> // IsMouseDragPastThreshold / IsDragDropActive

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
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
#include "utils/TomlConfig.hpp"
#include "utils/SettingsService.hpp"
#include "utils/Profiler.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEntities.hpp"

namespace aether::app
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
			for (TextureHandle h: {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
			{
				if (h.IsValid())
				{
					textures.Release(h);
				}
			}
		}

		bool AssignMaterialPreset(LayerContext& context, World& world, Entity entity, std::string_view path)
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
			MaterialAsset material = std::move(loaded.value());
			MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), material);
			ReleaseMaterialAssetTextures(*assets, material);
			if (auto* db = context.TryGet<AssetDatabase>())
			{
				db->Register(MakeMaterialPresetSource(std::string(path)));
			}
			return true;
		}

		bool AssignTextureToEntity(LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			if (assets == nullptr)
			{
				return false;
			}
			TextureHandle texture = assets->GetTextureRegistry().Acquire(path);
			MaterialSystem::SetAlbedoTexture(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), texture);
			if (texture.IsValid())
			{
				assets->GetTextureRegistry().Release(texture);
			}
			return true;
		}

		Entity InstantiatePrefabAsset(LayerContext& context, World& world, const std::string& name, Entity parent = {})
		{
			if (const auto prefab = scene::ReadPrefabFile(name))
			{
				const Entity root = scene::InstantiatePrefab(*prefab, world, scene::MakeApplySceneDeps(context.services), glm::mat4(1.0f));
				if (root.IsValid() && parent.IsValid())
				{
					ecs::SetParent(world, root, parent);
				}
				return root;
			}
			return {};
		}

		bool AssignModelToEntity(LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			auto* sceneCtx = context.TryGet<scripting::SceneContext>();
			if (assets == nullptr || sceneCtx == nullptr)
			{
				return false;
			}
			const bool ok = scene::AssignModelToEntity(world, *assets, *sceneCtx, entity, std::string(path));
			if (ok)
			{
				if (auto* db = context.TryGet<AssetDatabase>())
				{
					scene::RegisterModelAssets(*db, *assets, *sceneCtx, std::string(path));
				}
			}
			return ok;
		}

		bool ApplyFilePayloadToEntity(LayerContext& context, World& world, SceneSelection& selection, Entity entity, const dragdrop::FilePayload& payload)
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
				default:
					return false;
			}
		}

		bool CanApplyFilePayloadInHierarchy(const dragdrop::FilePayload& payload)
		{
			return payload.kind == dragdrop::FileKind::Prefab || payload.kind == dragdrop::FileKind::Material || payload.kind == dragdrop::FileKind::Texture || payload.kind == dragdrop::FileKind::Model;
		}

		// Small icon toggle used for the kind-filter chips.
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
			drawList->AddRectFilled(min, max, IM_COL32(31, 34, 40, 238), 5.0f);
			drawList->AddRect(min, max, IM_COL32(105, 170, 255, 185), 5.0f, 0, 1.0f);
			drawList->AddText(textPos, ImGui::ColorConvertFloat4ToU32(badge.color), badge.icon);
			drawList->AddText(ImVec2(textPos.x + iconSize.x + gap.x, textPos.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
			drawList->AddText(ImVec2(textPos.x + iconSize.x + gap.x + nameSize.x, textPos.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), idText.c_str());
		}

		// Selection ROOTS: drop any entity whose ancestor is also selected (it
		// rides along with the ancestor). Shared by duplicate, copy and cut.
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

		// Origin-spawned primitive for the "+" menu; mirrors das create_mesh/add_mesh
		// defaults (neutral two-sided material) via the same AssignMaterial path.
		void CreatePrimitive(LayerContext& context, World& world, SceneSelection& selection, PrimitiveMesh kind, const char* name, const char* kindName)
		{
			auto* primitives = context.TryGet<PrimitiveMeshes>();
			auto* assets = context.TryGet<AssetManager>();
			if (!primitives || !assets)
			{
				return;
			}
			Entity e = world.Create();
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

	// Subtle stripes plus the two juice overlays (spawn flash, selection pulse),
	// drawn behind the row before its widgets so highlights and text sit on top.
	void HierarchyPanel::DrawRowBackdrop(const SceneSelection& selection, Entity e, int rowIndex)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 rowMin = ImGui::GetCursorScreenPos();
		const ImVec2 rowMax = ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ImGui::GetFrameHeight());
		const double now = ImGui::GetTime();

		if ((rowIndex & 1) == 1)
		{
			drawList->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 4));
		}

		if (selection.Contains(e))
		{
			drawList->AddRectFilled(rowMin, rowMax, IM_COL32(70, 135, 255, 72));
			drawList->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f, rowMax.y), IM_COL32(105, 170, 255, 220));
		}

		if (const auto it = m_spawnFlash.find(e.id); it != m_spawnFlash.end())
		{
			const float t = static_cast<float>((now - it->second) / 0.75);
			if (t < 1.0f)
			{
				const float eased = (1.0f - t) * (1.0f - t); // quadratic fade-out
				drawList->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f, 0.85f, 0.45f, eased * 0.30f)));
			}
		}

		if (m_pulseStart >= 0.0 && selection.Contains(e))
		{
			const float t = static_cast<float>((now - m_pulseStart) / 0.20);
			if (t < 1.0f)
			{
				const float eased = (1.0f - t) * (1.0f - t);
				drawList->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.30f, 0.62f, 1.00f, eased * 0.35f)));
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
				// collapse it yet - the press may start a multi-entity drag. The
				// collapse happens on release, only if no drag occurred.
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

	void HierarchyPanel::HandleRowDragDrop(LayerContext& context, World& world, SceneSelection& selection, Entity e, float dropMinY, float dropMaxY, float visualMaxX)
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

			DropZone zone;
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

			// Visual feedback while dragging over this item.
			if (ImGui::IsDragDropActive() && (hasScriptPayload || hasFilePayload || canDropHere))
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImU32 lineCol = IM_COL32(105, 170, 255, 230);
				const ImU32 fillCol = IM_COL32(70, 135, 255, 52);
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
				const bool canReorderOnRelease = draggedEntity != e && (!targetParent.IsValid() || (targetParent != draggedEntity && !ecs::IsAncestor(world, targetParent, draggedEntity)));
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
			Entity child = world.Create();
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
			m_pendingDuplicate = true; // applied after the walk (creates entities)
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
			// The name popup is begun at window level after the tree walk
			// (opening it from inside the row's context popup would nest).
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
				world.Emplace<DisabledComponent>(e);
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
				// Root-level siblings: all root entities.
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
		if (continuePreviousItem)
		{
			ImGui::SameLine();
		}

		// Inactive entities (self-disabled, or greyed by a disabled ancestor) are
		// dimmed across icon, name and id so a disabled subtree reads as a unit.
		const bool inactive = ecs::HasDisabledAncestor(world, e);
		ImVec4 iconColor = badge.color;
		if (inactive)
		{
			iconColor.w *= 0.4f;
		}
		ImGui::TextColored(iconColor, "%s", badge.icon);
		if (inactive)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 0.6f));
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
			std::string name = EntityDisplayName(world, e);
			if (searching && !needle.empty())
			{
				std::string lower = ToLower(name);
				const size_t pos = lower.find(needle);
				if (pos != std::string::npos)
				{
					if (pos > 0)
					{
						ImGui::TextUnformatted(name.substr(0, pos).c_str());
						ImGui::SameLine(0, 0);
					}
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
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
						const std::uint64_t siblingBit = 1ULL << static_cast<unsigned>(depth);
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
		// ArrowButton is drawn with FramePadding=(0,0), so its width is the font size.
		const float arrowCenter = ImGui::GetFontSize() * 0.5f;
		const float indentSp = ImGui::GetStyle().IndentSpacing + 6.0f;
		const ImU32 lineCol = IM_COL32(120, 145, 180, 155);
		const float cx = (rowMin.y + rowMax.y) * 0.5f;

		for (int d = 0; d + 1 < entry.depth; ++d)
		{
			if (entry.openMask & (1ULL << d))
			{
				const float vx = rowMin.x + static_cast<float>(d) * indentSp + arrowCenter;
				drawList->AddLine(ImVec2(vx, rowMin.y), ImVec2(vx, rowMax.y), lineCol, 1.25f);
			}
		}
		if (entry.depth > 0)
		{
			const float vx = rowMin.x + static_cast<float>(entry.depth - 1) * indentSp + arrowCenter;
			const float hxEnd = rowMin.x + static_cast<float>(entry.depth) * indentSp;
			const bool parentHasMoreSiblings = (entry.openMask & (1ULL << static_cast<unsigned>(entry.depth - 1))) != 0;
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
		const bool inactive = ecs::HasDisabledAncestor(world, e); // self or ancestor
		const bool hidden = world.Has<HiddenTag>(e);
		const bool notPickable = world.Has<NotPickableTag>(e);

		// Active/disable (power), visibility (eye) and pickability (padlock). The
		// power icon is red when this entity is explicitly disabled, muted when it
		// is only greyed by a disabled ancestor, and normal when active.
		struct Toggle
		{
			const char* icon;
			ImVec4 color;
			const char* tip;
			int kind; // 0 = disable, 1 = hidden, 2 = pickable
		};
		const Toggle toggles[3] = {
		        {ICON_FA_POWER_OFF, selfDisabled ? ImVec4(0.86f, 0.45f, 0.40f, 0.95f) : (inactive ? ImVec4(0.45f, 0.45f, 0.45f, 0.5f) : ImVec4(0.8f, 0.8f, 0.8f, 0.8f)), selfDisabled ? "Enable entity" : "Disable entity (and children)", 0},
		        {ICON_FA_EYE, hidden ? ImVec4(0.35f, 0.35f, 0.35f, 0.35f) : ImVec4(0.8f, 0.8f, 0.8f, 0.8f), hidden ? "Show in Scene View" : "Hide in Scene View", 1},
		        {notPickable ? ICON_FA_LOCK : ICON_FA_UNLOCK, notPickable ? ImVec4(0.35f, 0.35f, 0.35f, 0.35f) : ImVec4(0.8f, 0.8f, 0.8f, 0.8f), notPickable ? "Allow picking in Scene View" : "Disable picking in Scene View", 2},
		};

		// Right-align the group so the three buttons hug the row's trailing edge.
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
						world.Emplace<DisabledComponent>(e);
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

	// One row of the outliner: flat Selectable row with depth-based indent,
	// expand/collapse arrow, badge, name and muted id drawn inline.
	void HierarchyPanel::DrawNode(LayerContext& context, World& world, SceneSelection& selection, Entity e, int depth, int flatTreeIndex, bool searching, std::string_view needle)
	{
		const auto* h = world.TryGet<HierarchyComponent>(e);
		const bool hasKids = h && !h->children.empty();
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
			ImGui::GetWindowDrawList()->AddRectFilled(rowMin, visualRowMax, IM_COL32(255, 255, 255, 16));
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

		// Tree guide lines.
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
			const ImU32 arrowColor = arrowHovered ? ImGui::GetColorU32(ImGuiCol_Text) : IM_COL32(220, 216, 204, 210);
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

		// Right-aligned utility toggles (eye / padlock).
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

	void HierarchyPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();
		World& world = context.Get<World>();
		auto& selection = context.Get<SceneSelection>();
		auto& reg = world.GetRegistry();

		m_rowsPrev = std::move(m_rowsCur);
		m_rowsCur.clear();

		// A release anywhere retires any pending collapse the row handlers did
		// not consume this frame (e.g. the mouse was released off-row or after a
		// drag) so a stale press can never collapse a later selection.
		if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m_pendingCollapse.IsValid())
		{
			m_pendingCollapse = {};
		}

		// ── Juice bookkeeping ──────────────────────────────────────────────────
		const double now = ImGui::GetTime();
		{
			// Rebuilding the id set each frame keeps recycled ids flashing too.
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
			// External requests (File > Open / Save As, Ctrl+S fallback) open the
			// same popups the toolbar buttons below do, from this window's ID scope
			// so BeginPopup(...) below actually sees them.
			if (m_requestSaveAsPopup)
			{
				m_requestSaveAsPopup = false;
				ImGui::OpenPopup("SaveScene");
			}
			if (m_requestOpenPopup)
			{
				m_requestOpenPopup = false;
				m_sceneListDirty = true;
				ImGui::OpenPopup("LoadScene");
			}

			// ── Toolbar ────────────────────────────────────────────────────────
			if (ImGui::Button(ICON_FA_PLUS))
			{
				ImGui::OpenPopup("CreateEntity");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Create entity");
			}
			if (ImGui::BeginPopup("CreateEntity"))
			{
				if (ImGui::MenuItem(ICON_FA_CIRCLE "  Empty entity"))
				{
					Entity e = world.Create();
					world.Emplace<NameComponent>(e, NameComponent{.name = "Entity"});
					selection.Select(e);
				}
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
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Point Light"))
				{
					selection.Select(ecs::CreatePointLightEntity(world, {0.0f, 5.0f, 0.0f}, PointLightComponent{}));
				}
				if (ImGui::MenuItem(ICON_FA_LIGHTBULB "  Spot Light"))
				{
					// Spawn aimed forward-down so the cone lands in front of you.
					selection.Select(ecs::CreateSpotLightEntity(world, {0.0f, 8.0f, 0.0f}, {0.0f, -0.85f, -0.5f}, SpotLightComponent{}));
				}
				ImGui::Separator();
				if (ImGui::MenuItem(ICON_FA_VIDEO "  Camera"))
				{
					// Spawn back-and-up looking toward the origin, so it frames the
					// scene the way the default editor view does.
					const Entity cam = ecs::CreateCameraEntity(world, {0.0f, 3.0f, 8.0f}, {0.0f, -0.35f, -1.0f}, CameraComponent{});
					// First camera created becomes the main camera for convenience.
					if (!ecs::GetMainCameraEntity(world).IsValid())
					{
						ecs::SetMainCameraEntity(world, cam);
					}
					selection.Select(cam);
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

			// Scene save/load.
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_FLOPPY_DISK))
			{
				ImGui::OpenPopup("SaveScene");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Save scene");
			}
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_FOLDER_OPEN))
			{
				m_sceneListDirty = true;
				ImGui::OpenPopup("LoadScene");
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Load scene (replaces all entities)");
			}
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_ROTATE))
			{
				m_sceneListDirty = true;
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
			{
				ImGui::SetTooltip("Refresh scenes");
			}

			if (ImGui::BeginPopup("SaveScene"))
			{
				ImGui::SetNextItemWidth(180.0f);
				const bool entered = ImGui::InputTextWithHint("##sceneName", "Scene name...", m_sceneNameBuf, sizeof(m_sceneNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
				ImGui::SameLine();
				const bool save = ImGui::Button(ICON_FA_FLOPPY_DISK " Save") || entered;
				ImGui::TextDisabled("-> %s", scene::ScenesDirectory().c_str());
				if (save && m_sceneNameBuf[0] != '\0')
				{
					if (auto* assets = context.TryGet<AssetManager>())
					{
						const auto captured = scene::CaptureScene(world, assets->GetMaterialRegistry(), assets->GetTextureRegistry(), context.TryGet<Renderer>());
						scene::SaveSceneFile(m_sceneNameBuf, captured);
						m_sceneListDirty = true;
						if (auto* scenes = context.TryGet<aether::SceneSubsystem>())
						{
							scenes->SetCurrentScene(m_sceneNameBuf);
						}
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			if (ImGui::BeginPopup("LoadScene"))
			{
				if (m_sceneListDirty)
				{
					m_sceneList = scene::ListSceneFiles();
					m_sceneListDirty = false;
				}
				if (m_sceneList.empty())
				{
					ImGui::TextDisabled("No scenes in %s", scene::ScenesDirectory().c_str());
				}
				auto* settingsService = context.TryGet<aether::SettingsService>();

				// Fixed name-column width keeps the row layout (and thus the popup
				// width) stable. Deriving the button X from GetContentRegionMax()
				// inside an auto-sizing popup feedback-loops the window wider each frame.
				float nameColWidth = ImGui::CalcTextSize("startup").x;
				for (const std::string& name: m_sceneList)
				{
					const float w = ImGui::CalcTextSize(name.c_str()).x;
					if (w > nameColWidth)
					{
						nameColWidth = w;
					}
				}
				nameColWidth += ImGui::GetStyle().ItemSpacing.x + 8.0f;

				for (const std::string& name: m_sceneList)
				{
					ImGui::PushID(name.c_str());
					const bool isStartup = settingsService != nullptr && settingsService->Get().app.startupScene == name;
					// A full-width MenuItem swallowed clicks meant for the trailing
					// startup button. A fixed-width Selectable with AllowOverlap keeps
					// the button clickable and the popup a stable width; Selectable does
					// not auto-close, so do it here.
					if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(nameColWidth, 0.0f)))
					{
						if (scene::LoadSceneFile(name, world, scene::MakeApplySceneDeps(context.services)))
						{
							selection.Clear();
							if (auto* scenes = context.TryGet<aether::SceneSubsystem>())
							{
								scenes->SetCurrentScene(name);
							}
						}
						ImGui::CloseCurrentPopup();
					}
					if (settingsService != nullptr)
					{
						ImGui::SameLine();
						if (isStartup)
						{
							ImGui::TextDisabled("startup");
						}
						else
						{
							if (ImGui::SmallButton(ICON_FA_PLAY "##startup"))
							{
								// Edit through the single source of truth and persist the
								// user delta immediately (startupScene has no live effect;
								// it takes hold on next launch).
								settingsService->Values().app.startupScene = name;
								settingsService->ApplyField("app.startupScene");
								settingsService->Save();
							}
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
							{
								ImGui::SetTooltip("Set as startup scene (saves to user settings)");
							}
						}
					}
					ImGui::PopID();
				}
				ImGui::EndPopup();
			}

			// Save-as-prefab (armed by the row context menu during the walk;
			// fires here at window level the next frame).
			if (m_openPrefabSave)
			{
				ImGui::OpenPopup("SavePrefab");
				m_openPrefabSave = false;
			}
			if (ImGui::BeginPopup("SavePrefab"))
			{
				auto& reg2 = world.GetRegistry();
				if (!m_prefabSaveTarget.IsValid() || !reg2.valid(World::ToEntt(m_prefabSaveTarget)))
				{
					ImGui::CloseCurrentPopup();
				}
				else
				{
					ImGui::TextDisabled("Prefab of '%s' (subtree)", EntityDisplayName(world, m_prefabSaveTarget));
					ImGui::SetNextItemWidth(180.0f);
					const bool entered = ImGui::InputTextWithHint("##prefabName", "Prefab name...", m_prefabNameBuf, sizeof(m_prefabNameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
					ImGui::SameLine();
					const bool save = ImGui::Button(ICON_FA_FLOPPY_DISK " Save") || entered;
					ImGui::TextDisabled("-> %s", scene::PrefabsDirectory().c_str());
					if (save && m_prefabNameBuf[0] != '\0')
					{
						if (auto* assets = context.TryGet<AssetManager>())
						{
							scene::SavePrefabFile(m_prefabNameBuf, scene::CapturePrefab(world, m_prefabSaveTarget, assets->GetMaterialRegistry(), assets->GetTextureRegistry()));
						}
						ImGui::CloseCurrentPopup();
					}
				}
				ImGui::EndPopup();
			}

			ImGui::SameLine();
			ImGui::TextDisabled(ICON_FA_MAGNIFYING_GLASS);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##search", "Search name or #id...", m_search, sizeof(m_search));

			// Kind chips + right-aligned live count on one row.
			FilterChip(ICON_FA_CUBE, "Meshes", ImVec4(0.62f, 0.88f, 0.62f, 1.0f), m_filterMesh);
			ImGui::SameLine();
			FilterChip(ICON_FA_PERSON_RUNNING, "Skinned", ImVec4(0.55f, 0.75f, 1.00f, 1.0f), m_filterSkinned);
			ImGui::SameLine();
			FilterChip(ICON_FA_WEIGHT_HANGING, "Physics", ImVec4(1.00f, 0.72f, 0.35f, 1.0f), m_filterPhysics);
			ImGui::SameLine();
			FilterChip(ICON_FA_WAND_MAGIC_SPARKLES, "Effects", ImVec4(0.80f, 0.55f, 1.00f, 1.0f), m_filterEffect);

			std::size_t count = 0;
			for (const auto handle: reg.storage<entt::entity>())
			{
				if (reg.valid(handle) && World::FromEntt(handle).IsValid())
				{
					++count;
				}
			}
			char countText[32]{};
			std::snprintf(countText, sizeof(countText), "%zu entities", count);
			ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(countText).x);
			ImGui::TextDisabled("%s", countText);

			// Sync persistent expansion from settings once the world is ready.
			if (!m_expandedPathsLoaded)
			{
				SyncExpandedFromPaths(world);
			}

			// ── Breadcrumb trail ────────────────────────────────────────────────
			DrawBreadcrumbTrail(world, selection);

			// ── Body ───────────────────────────────────────────────────────────
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
				// Flat, clipper-friendly result list.
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
						if (!ContainsCaseInsensitive(EntityDisplayName(world, e), needle) && std::string_view(idText).find(needle) == std::string::npos)
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
						// Same interaction path as tree rows: modifier-aware click
						// (ctrl/shift/deferred collapse) + multi-entity drag-drop.
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
				// Build flat visible tree for clipper-friendly iteration.
				m_flatTree.clear();
				const auto& roots = world.Roots();
				for (std::size_t i = 0; i < roots.size(); ++i)
				{
					const Entity e = roots[i];
					if (e.IsValid() && reg.valid(World::ToEntt(e)))
					{
						const std::uint64_t openMask = (i + 1 < roots.size()) ? 1ULL : 0ULL;
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

				// Small tail drop target: drop here to detach to root without
				// consuming the asset browser area below the scene rows.
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

			// ── Keyboard navigation + type-to-jump ────────────────────────────
			if (!filtering && SceneListOwnsKeyboard())
			{
				HandleKeyboardNavigation(selection);
				HandleTypeToJump(world, selection);
			}

			// ── Clipboard + duplicate (EDITOR-GLOBAL edit shortcuts) ──────────
			// The selection is the context, not window focus: Ctrl+D then a
			// gizmo drag then Ctrl+D again must work without re-clicking the
			// outliner. Only active text input suppresses them (this panel
			// runs every frame, so evaluating global key state here is fine).
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

			// Copy/cut put the selection roots on the OS CLIPBOARD as scene
			// TOML - pasteable in this session, another session, or a text
			// editor. Paste applies additively, nudged +1 X.
			if ((copyKey || cutKey) && clipAssets != nullptr && !selection.All().empty())
			{
				const std::vector<Entity> roots = CollectSelectionRoots(world, selection);
				if (!roots.empty())
				{
					ImGui::SetClipboardText(scene::WriteToml(scene::CaptureSubtrees(world, roots, clipAssets->GetMaterialRegistry(), clipAssets->GetTextureRegistry())).c_str());
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
					if (const auto parsed = scene::ParseToml(clip); parsed.has_value() && !parsed->entities.empty())
					{
						if (undo != nullptr)
						{
							undo->Push(world, context.services);
						}
						const auto created = scene::ApplyScene(*parsed, world, scene::MakeApplySceneDeps(context.services));
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

			// Ctrl+D duplicates the selection (roots only; nested selected
			// entities ride with their ancestor's copy).
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
					// A duplicate is an in-memory prefab round trip: capture the
					// subtree, instantiate nudged +1 X, select the copies.
					bool first = true;
					for (const Entity root: roots)
					{
						const auto prefab = scene::CapturePrefab(world, root, dupAssets->GetMaterialRegistry(), dupAssets->GetTextureRegistry());
						glm::mat4 placed(1.0f);
						if (const auto* tc = world.TryGet<TransformComponent>(root))
						{
							placed = tc->localToWorld;
						}
						placed[3].x += 1.0f;
						const Entity copy = scene::InstantiatePrefab(prefab, world, scene::MakeApplySceneDeps(context.services), placed);
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

			// Apply the queued reparent now that no children vector is being
			// walked. SetParent is cycle-guarded: bad drops are silent no-ops.
			if (m_pendingReparent)
			{
				const PendingReparent pr = *m_pendingReparent;
				m_pendingReparent.reset();
				if (world.GetRegistry().valid(World::ToEntt(pr.child)))
				{
					// Dragging a selected row moves the WHOLE selection - but only
					// its topmost roots: an entity whose ancestor is also selected
					// follows that ancestor, preserving structure inside the
					// selection (and the drop target itself is a no-op via the
					// child==parent guard).
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

			// ── Keyboard ───────────────────────────────────────────────────────
			// Delete is editor-global like the clipboard shortcuts: it acts on
			// the shared selection from any window (undo covers slips).
			if (!ImGui::GetIO().WantTextInput)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !selection.All().empty())
				{
					if (auto* undoStack = context.TryGet<UndoStack>())
					{
						undoStack->Push(world, context.services);
					}
					// Copy first: DestroyHierarchy mutates the selection source, and
					// an earlier subtree delete may swallow later selected entities.
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
				// F2 stays window-scoped: it opens the outliner's inline editor.
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F2) && selection.Primary().IsValid())
				{
					BeginRename(world, selection.Primary());
				}
			}
		}
		ImGui::End();
	}

	// ── AAA feature implementations ─────────────────────────────────────

	// Walk up the parent chain building a "/Root/Child/Grandchild"-style path.
	std::string HierarchyPanel::ComputeEntityPath(const World& world, Entity e) const
	{
		std::vector<std::string> segments;
		Entity cur = e;
		while (cur.IsValid())
		{
			segments.push_back(EntityDisplayName(world, cur));
			const auto* h = world.TryGet<HierarchyComponent>(cur);
			cur = h ? h->parent : Entity{};
		}
		std::string path;
		for (auto it = segments.rbegin(); it != segments.rend(); ++it)
		{
			path += '/';
			path += *it;
		}
		return path;
	}

	// Try to find an entity by walking from world root entities along the
	// given path segments (e.g. {"Root", "Child", "Grandchild"}).
	// Returns invalid entity on failure.
	namespace
	{
		Entity FindEntityByPath(const World& world, const std::vector<std::string>& segments)
		{
			if (segments.empty())
			{
				return {};
			}
			// Search root-level entities for the first segment.
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
			// Strip leading '/'.
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

	// Populate m_expandedNodes from the path-based m_expandedPaths map.
	// Should be called once after the scene is populated.
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

		// Collect ancestor chain from root to leaf.
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
			ImGui::PushStyleColor(ImGuiCol_Text, isPrimary ? ImGui::GetStyle().Colors[ImGuiCol_Text] : ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
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

		// Reset on timeout.
		if (!m_typeJumpText.empty() && (now - m_typeJumpTime) > kTypeTimeout)
		{
			m_typeJumpText.clear();
		}

		// Accumulate printable characters.
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

		// Find first entity whose name starts with the typed text.
		const auto& rows = m_rowsCur;
		for (std::size_t i = 0; i < rows.size(); ++i)
		{
			const std::string name = EntityDisplayName(world, rows[i]);
			std::string lower;
			lower.reserve(name.size());
			for (unsigned char c: name)
			{
				lower.push_back(static_cast<char>(std::tolower(c)));
			}
			if (lower.starts_with(m_typeJumpText))
			{
				selection.Select(rows[i]);
				m_scrollToEntity = rows[i];
				break;
			}
		}
	}

	// --- Settings persistence ---

	void HierarchyPanel::LoadSettings(TomlConfig& config, LayerContext& context)
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
				// Store a flag so we'll look this up later when the scene is loaded.
				// The path string is embedded in the key itself for this simple
				// flat config system.
				m_expandedPaths[std::string(key)] = true;
			}
		}
		m_expandedPathsLoaded = false; // will be synced on first OnImGui with a valid world
	}

	void HierarchyPanel::SaveSettings(TomlConfig& config, LayerContext& context) const
	{
		(void) context;
		config.Set("debug.hierarchy.filterMesh", m_filterMesh);
		config.Set("debug.hierarchy.filterSkinned", m_filterSkinned);
		config.Set("debug.hierarchy.filterPhysics", m_filterPhysics);
		config.Set("debug.hierarchy.filterEffect", m_filterEffect);
	}
} // namespace aether::app
