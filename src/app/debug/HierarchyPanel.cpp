#include "debug/HierarchyPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	namespace
	{
		const char* EntityDisplayName(const World& world, Entity e)
		{
			const auto* nc = world.TryGet<NameComponent>(e);
			return (nc && !nc->name.empty()) ? nc->name.c_str() : "Entity";
		}

		// Colored icon describing the entity's dominant kind (most specific wins).
		struct KindBadge
		{
			const char* icon;
			ImVec4 color;
		};

		KindBadge BadgeFor(const World& world, Entity e)
		{
			if (world.Has<SkinnedMeshComponent>(e))
			{
				return {ICON_FA_PERSON_RUNNING, ImVec4(0.55f, 0.75f, 1.00f, 1.0f)};
			}
			if (world.Has<EffectParamsComponent>(e))
			{
				return {ICON_FA_WAND_MAGIC_SPARKLES, ImVec4(0.80f, 0.55f, 1.00f, 1.0f)};
			}
			if (world.Has<RigidBodyComponent>(e))
			{
				return {ICON_FA_WEIGHT_HANGING, ImVec4(1.00f, 0.72f, 0.35f, 1.0f)};
			}
			if (world.Has<MeshComponent>(e))
			{
				return {ICON_FA_CUBE, ImVec4(0.62f, 0.88f, 0.62f, 1.0f)};
			}
			return {ICON_FA_CIRCLE, ImVec4(0.50f, 0.50f, 0.50f, 1.0f)};
		}

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

		// Origin-spawned primitive for the "+" menu; mirrors das create_mesh/add_mesh
		// defaults (neutral two-sided material) via the same AssignMaterial path.
		void CreatePrimitive(LayerContext& context, World& world, SceneSelection& selection, PrimitiveMesh kind, const char* name)
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
			MaterialAsset asset{};
			asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.f);
			asset.roughnessFactor = 0.6f;
			asset.metallicFactor = 0.0f;
			asset.doubleSided = true;
			MaterialSystem::AssignMaterial(world, e, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			selection.Select(e);
		}
	} // namespace

	void HierarchyPanel::BeginRename(const World& world, Entity e)
	{
		m_renaming = e;
		m_renameFocusPending = true;
		std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", EntityDisplayName(world, e));
	}

	void HierarchyPanel::HandleRowClick(SceneSelection& selection, Entity e)
	{
		if (!ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemToggledOpen())
		{
			return;
		}
		const ImGuiIO& io = ImGui::GetIO();
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
		else
		{
			selection.Select(e);
			m_rangeAnchor = e;
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
		const KindBadge badge = BadgeFor(world, e);
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
		const bool open = ImGui::TreeNodeEx("##node", flags);
		m_rowsCur.push_back(e);
		HandleRowClick(selection, e);
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

		ImGui::Begin("Scene");
		{
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
					CreatePrimitive(context, world, selection, PrimitiveMesh::Cube, "Cube");
				}
				if (ImGui::MenuItem(ICON_FA_CIRCLE "  Sphere"))
				{
					CreatePrimitive(context, world, selection, PrimitiveMesh::Sphere, "Sphere");
				}
				if (ImGui::MenuItem(ICON_FA_IMAGE "  Plane"))
				{
					CreatePrimitive(context, world, selection, PrimitiveMesh::Plane, "Plane");
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
						if (ImGui::Selectable("##row", selection.Contains(e), ImGuiSelectableFlags_SpanAllColumns))
						{
							// Selectable consumed the click; route modifiers manually.
							const ImGuiIO& io = ImGui::GetIO();
							if (io.KeyCtrl)
							{
								selection.ToggleSelection(e);
							}
							else
							{
								selection.Select(e);
							}
							m_rangeAnchor = e;
						}
						m_rowsCur.push_back(e);
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
			}
			ImGui::EndChild();

			// ── Keyboard (window-scope) ────────────────────────────────────────
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput)
			{
				if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !selection.All().empty())
				{
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
				if (ImGui::IsKeyPressed(ImGuiKey_F2) && selection.Primary().IsValid())
				{
					BeginRename(world, selection.Primary());
				}
			}
		}
		ImGui::End();
	}
} // namespace aether::app
