#include "debug/InspectorPanel.hpp"

#include <cctype>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "debug/ComponentDrawers.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	namespace
	{
		// One filterable row of the add-component palette.
		bool PaletteEntry(const char* label, const char* filter, bool alreadyPresent)
		{
			if (alreadyPresent)
			{
				return false;
			}
			if (filter[0] != '\0')
			{
				const std::string_view l(label);
				std::string lower(l);
				std::string needle(filter);
				for (auto& c: lower)
				{
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				for (auto& c: needle)
				{
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				if (lower.find(needle) == std::string::npos)
				{
					return false;
				}
			}
			return ImGui::MenuItem(label);
		}
	} // namespace

	bool InspectorPanel::IsAlive(const World& world, Entity entity)
	{
		return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
	}

	void InspectorPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Inspector");
		World& world = context.Get<World>();
		auto& selection = context.Get<SceneSelection>();
		const Entity entity = selection.Primary();

		if (!IsAlive(world, entity))
		{
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.4f));
			const char* msg = "Nothing selected";
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) * 0.5f);
			ImGui::TextDisabled("%s", msg);
			ImGui::End();
			return;
		}

		// ── Header: kind icon, editable name, muted id ─────────────────────────
		const KindBadge badge = EntityKindBadge(world, entity);
		ImGui::TextColored(badge.color, "%s", badge.icon);
		ImGui::SameLine();
		if (auto* nc = world.TryGet<NameComponent>(entity))
		{
			char buf[128];
			std::snprintf(buf, sizeof(buf), "%s", nc->name.c_str());
			ImGui::SetNextItemWidth(-64.0f);
			if (ImGui::InputText("##name", buf, sizeof(buf)))
			{
				nc->name = buf;
			}
		}
		else
		{
			if (ImGui::SmallButton("Add name"))
			{
				world.Emplace<NameComponent>(entity, NameComponent{.name = "Entity"});
			}
		}
		ImGui::SameLine();
		ImGui::TextDisabled("#%u", entity.id);

		if (selection.All().size() > 1)
		{
			ImGui::TextDisabled("Editing primary of %zu selected", selection.All().size());
		}

		// ── Add Component palette + delete entity ──────────────────────────────
		if (ImGui::Button(ICON_FA_PLUS "  Add Component"))
		{
			m_addFilter[0] = '\0';
			m_addFocusPending = true;
			ImGui::OpenPopup("AddComponent");
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
		const bool deleteClicked = ImGui::Button(ICON_FA_TRASH "  Delete");
		ImGui::PopStyleColor();
		if (deleteClicked)
		{
			ecs::DestroyHierarchy(world, entity);
			selection.Clear();
			ImGui::End();
			return;
		}

		if (ImGui::BeginPopup("AddComponent"))
		{
			if (m_addFocusPending)
			{
				ImGui::SetKeyboardFocusHere();
				m_addFocusPending = false;
			}
			ImGui::SetNextItemWidth(200.0f);
			ImGui::InputTextWithHint("##addFilter", "Search...", m_addFilter, sizeof(m_addFilter));
			ImGui::Separator();
			// Safely default-constructible components only; asset-bearing ones
			// (Mesh/Pipeline/Material) need a picker - later spec.
			if (PaletteEntry(ICON_FA_UP_DOWN_LEFT_RIGHT "  Transform", m_addFilter, world.Has<TransformComponent>(entity)))
			{
				world.Emplace<TransformComponent>(entity);
			}
			if (PaletteEntry(ICON_FA_PEN "  Name", m_addFilter, world.Has<NameComponent>(entity)))
			{
				world.Emplace<NameComponent>(entity, NameComponent{.name = "Entity"});
			}
			if (PaletteEntry(ICON_FA_SITEMAP "  Hierarchy", m_addFilter, world.Has<HierarchyComponent>(entity)))
			{
				world.Emplace<HierarchyComponent>(entity);
			}
			ImGui::EndPopup();
		}
		ImGui::Separator();

		// ── Component sections ─────────────────────────────────────────────────
		DrawTransform(context, world, entity);
		DrawSkinnedMesh(world, entity);
		DrawMaterial(context, world, entity);
		DrawEffectParams(context, world, entity);
		DrawPhysics(world, entity);
		DrawMeshPipeline(world, entity);
		DrawHierarchy(world, entity, selection);
		DrawTags(world, entity, m_addTagBuf, sizeof(m_addTagBuf));

		ImGui::End();
	}
} // namespace aether::app
