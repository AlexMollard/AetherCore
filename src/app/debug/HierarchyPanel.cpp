#include "debug/HierarchyPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <imgui.h>
#include <imgui_internal.h> // IsMouseDragPastThreshold / IsDragDropActive

#include "assets/AssetManager.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "io/FileSystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "material/EffectParamBuffer.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "rendering/Renderer.hpp"
#include "debug/UndoStack.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/EngineSettings.hpp"
#include "utils/SettingsService.hpp"
#include "utils/Profiler.hpp"

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

		// Selection ROOTS: drop any entity whose ancestor is also selected (it
		// rides along with the ancestor). Shared by duplicate, copy and cut.
		std::vector<Entity> CollectSelectionRoots(World& world, const SceneSelection& selection)
		{
			std::vector<Entity> roots;
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
	} // namespace

	// Subtle stripes plus the two juice overlays (spawn flash, selection pulse),
	// drawn behind the row before its widgets so highlights and text sit on top.
	void HierarchyPanel::DrawRowBackdrop(const SceneSelection& selection, Entity e)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 rowMin = ImGui::GetCursorScreenPos();
		const ImVec2 rowMax = ImVec2(rowMin.x + ImGui::GetContentRegionAvail().x, rowMin.y + ImGui::GetFrameHeight());
		const double now = ImGui::GetTime();

		if ((m_rowsCur.size() & 1) == 1)
		{
			drawList->AddRectFilled(rowMin, rowMax, IM_COL32(255, 255, 255, 4));
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
			if (io.KeyCtrl)
			{
				selection.ToggleSelection(e);
				m_rangeAnchor = e;
			}
			else if (io.KeyShift && m_rangeAnchor.IsValid())
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
			else
			{
				// Pressing a row that is part of a multi-selection must NOT
				// collapse it yet - the press may start a multi-entity drag. The
				// collapse happens on release, only if no drag occurred.
				m_pendingCollapse = e;
			}
		}

		if (m_pendingCollapse == e && inMultiSelection && !io.KeyCtrl && !io.KeyShift && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 4.0f)
		        && !ImGui::IsDragDropActive())
		{
			selection.Select(e);
			m_rangeAnchor = e;
			m_pendingCollapse = {};
		}
	}

	void HierarchyPanel::HandleRowDragDrop(World& world, SceneSelection& selection, Entity e)
	{
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
		{
			ImGui::SetDragDropPayload("AETHER_ENTITY", &e.id, sizeof(e.id));
			// Dragging a selected row carries the whole selection.
			const std::size_t count = (selection.Contains(e) && selection.All().size() > 1) ? selection.All().size() : 1;
			if (count > 1)
			{
				ImGui::Text("Move %zu entities", count);
			}
			else
			{
				ImGui::TextUnformatted(EntityDisplayName(world, e));
			}
			ImGui::TextDisabled("drop on a row to parent, empty space to unparent");
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AETHER_ENTITY"))
			{
				const auto draggedId = *static_cast<const std::uint32_t*>(p->Data);
				m_pendingReparent = {Entity{draggedId}, e};
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
		if (ImGui::MenuItem(ICON_FA_TRASH "  Delete (subtree)"))
		{
			ecs::DestroyHierarchy(world, e);
			selection.Clear();
			destroyed = true;
		}
		ImGui::EndPopup();
		return destroyed;
	}

	void HierarchyPanel::DrawRowContent(World& world, Entity e)
	{
		const KindBadge badge = EntityKindBadge(world, e);
		ImGui::SameLine();
		ImGui::TextColored(badge.color, "%s", badge.icon);

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
				}
				m_renaming = {};
			}
		}
		else
		{
			ImGui::SameLine();
			ImGui::TextUnformatted(EntityDisplayName(world, e));
		}
		ImGui::SameLine();
		ImGui::TextDisabled("#%u", e.id);
	}

	// One row of the outliner: tree node (arrow + full-row hit area) with the
	// badge, name and muted id drawn inline; recurses into children.
	void HierarchyPanel::DrawNode(World& world, SceneSelection& selection, Entity e)
	{
		const auto* h = world.TryGet<HierarchyComponent>(e);
		const bool hasKids = h && !h->children.empty();

		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DrawLinesToNodes;
		if (selection.Contains(e))
		{
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		if (!hasKids)
		{
			flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
		}

		ImGui::PushID(static_cast<int>(e.id));
		DrawRowBackdrop(selection, e);
		const bool open = ImGui::TreeNodeEx("##node", flags);
		m_rowsCur.push_back(e);
		HandleRowClick(selection, e);
		HandleRowDragDrop(world, selection, e);
		const bool destroyed = DrawRowContextMenu(world, selection, e);
		if (!destroyed)
		{
			DrawRowContent(world, e);
		}

		if (open && hasKids)
		{
			if (!destroyed)
			{
				// Re-fetch: creating a child inside the context menu may have
				// reallocated the HierarchyComponent pool.
				if (const auto* hNow = world.TryGet<HierarchyComponent>(e))
				{
					for (const Entity c: hNow->children)
					{
						DrawNode(world, selection, c);
					}
				}
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
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
			std::unordered_set<std::uint32_t> current;
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
				current.insert(e.id);
				if (m_knownSeeded && !m_knownIds.contains(e.id))
				{
					m_spawnFlash[e.id] = now;
				}
			}
			m_knownIds = std::move(current);
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
			// ── Toolbar ────────────────────────────────────────────────────────
			if (ImGui::Button(ICON_FA_PLUS))
			{
				// Refresh the asset lists once per open, not per frame.
				m_modelList.clear();
				if (const auto models = io::FileSystem::Glob("assets://models/**/*.mesh"); models.has_value())
				{
					m_modelList = *models;
					std::sort(m_modelList.begin(), m_modelList.end());
				}
				m_prefabList = scene::ListPrefabFiles();
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
				if (ImGui::BeginMenu(ICON_FA_PERSON_RUNNING "  Model"))
				{
					if (m_modelList.empty())
					{
						ImGui::TextDisabled("No .mesh files under assets://models");
					}
					auto* assets = context.TryGet<AssetManager>();
					auto* sceneCtx = context.TryGet<scripting::SceneContext>();
					for (const std::string& path: m_modelList)
					{
						std::string label = path;
						if (const auto slash = label.find_last_of("/\\"); slash != std::string::npos)
						{
							label = label.substr(slash + 1);
						}
						ImGui::PushID(path.c_str());
						if (ImGui::MenuItem(label.c_str()) && assets != nullptr && sceneCtx != nullptr)
						{
							const Entity root = scene::SpawnModelEntity(world, *assets, *sceneCtx, path, glm::mat4(1.0f));
							if (root.IsValid())
							{
								selection.Select(root);
							}
						}
						ImGui::PopID();
					}
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu(ICON_FA_BOX_OPEN "  Prefab"))
				{
					if (m_prefabList.empty())
					{
						ImGui::TextDisabled("No prefabs in %s", scene::PrefabsDirectory().c_str());
						ImGui::TextDisabled("(right-click an entity -> Save as Prefab)");
					}
					for (const std::string& name: m_prefabList)
					{
						ImGui::PushID(name.c_str());
						if (ImGui::MenuItem(name.c_str()))
						{
							if (const auto prefab = scene::ReadPrefabFile(name))
							{
								const Entity root = scene::InstantiatePrefab(*prefab, world, scene::MakeApplySceneDeps(context.services), glm::mat4(1.0f));
								if (root.IsValid())
								{
									selection.Select(root);
								}
							}
						}
						ImGui::PopID();
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
			const std::string countText = std::to_string(count) + " entities";
			ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize(countText.c_str()).x);
			ImGui::TextDisabled("%s", countText.c_str());

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

			ImGui::BeginChild("SceneList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			if (filtering)
			{
				// Flat, clipper-friendly result list.
				const std::string needle = ToLower(m_search);
				std::vector<Entity> matches;
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
						const std::string idText = "#" + std::to_string(e.id);
						if (ToLower(EntityDisplayName(world, e)).find(needle) == std::string::npos && idText.find(needle) == std::string::npos)
						{
							continue;
						}
					}
					matches.push_back(e);
				}

				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(matches.size()));
				while (clipper.Step())
				{
					for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					{
						const Entity e = matches[static_cast<std::size_t>(i)];
						ImGui::PushID(static_cast<int>(e.id));
						DrawRowBackdrop(selection, e);
						// Same interaction path as tree rows: modifier-aware click
						// (ctrl/shift/deferred collapse) + multi-entity drag-drop.
						ImGui::Selectable("##row", selection.Contains(e), ImGuiSelectableFlags_SpanAllColumns);
						m_rowsCur.push_back(e);
						HandleRowClick(selection, e);
						HandleRowDragDrop(world, selection, e);
						if (!DrawRowContextMenu(world, selection, e))
						{
							DrawRowContent(world, e);
						}
						ImGui::PopID();
					}
				}
				if (matches.empty())
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
				// Roots: no HierarchyComponent, or an explicit root parent ({0}).
				for (const auto handle: reg.storage<entt::entity>())
				{
					if (!reg.valid(handle))
					{
						continue;
					}
					const Entity e = World::FromEntt(handle);
					if (!e.IsValid())
					{
						continue; // id 0 is the null entity, never listed
					}
					const auto* h = world.TryGet<HierarchyComponent>(e);
					if (!h || !h->parent.IsValid())
					{
						DrawNode(world, selection, e);
					}
				}

				// Remaining empty area: drop here to detach to root.
				const float remaining = ImGui::GetContentRegionAvail().y;
				if (remaining > 4.0f)
				{
					ImGui::InvisibleButton("##emptyDrop", ImVec2(ImGui::GetContentRegionAvail().x, remaining));
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AETHER_ENTITY"))
						{
							const auto draggedId = *static_cast<const std::uint32_t*>(p->Data);
							m_pendingReparent = {Entity{draggedId}, Entity{}};
						}
						ImGui::EndDragDropTarget();
					}
				}
			}
			ImGui::EndChild();

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
				const auto [dragged, parent] = *m_pendingReparent;
				m_pendingReparent.reset();
				if (world.GetRegistry().valid(World::ToEntt(dragged)))
				{
					// Dragging a selected row moves the WHOLE selection - but only
					// its topmost roots: an entity whose ancestor is also selected
					// follows that ancestor, preserving structure inside the
					// selection (and the drop target itself is a no-op via the
					// child==parent guard).
					std::vector<Entity> moved;
					if (selection.Contains(dragged) && selection.All().size() > 1)
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
						moved.push_back(dragged);
					}
					for (const Entity e: moved)
					{
						ecs::SetParent(world, e, parent);
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
} // namespace aether::app
