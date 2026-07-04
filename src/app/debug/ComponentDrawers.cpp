#include "debug/ComponentDrawers.hpp"

#include <algorithm>
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

namespace aether::app
{
	namespace
	{
		// Unity-style vector row: colored axis chip (click resets that axis) +
		// per-axis drag. Returns true when any component changed.
		bool DrawVec3Row(const char* label, glm::vec3& value, float resetValue, float speed)
		{
			bool changed = false;
			ImGui::PushID(label);

			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			ImGui::SameLine(86.0f);

			struct AxisChip
			{
				const char* tag;
				ImVec4 color;
				float* component;
			};
			AxisChip axes[3] = {
			        {"X", ImVec4(0.79f, 0.29f, 0.32f, 1.0f), &value.x},
			        {"Y", ImVec4(0.38f, 0.64f, 0.31f, 1.0f), &value.y},
			        {"Z", ImVec4(0.26f, 0.50f, 0.83f, 1.0f), &value.z},
			};

			const float chipWidth = ImGui::GetFrameHeight();
			const float spacing = 4.0f;
			const float fieldWidth = std::max(42.0f, (ImGui::GetContentRegionAvail().x - 3.0f * chipWidth - 2.0f * spacing) / 3.0f);

			for (int i = 0; i < 3; ++i)
			{
				if (i > 0)
				{
					ImGui::SameLine(0.0f, spacing);
				}
				ImGui::PushStyleColor(ImGuiCol_Button, axes[i].color);
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(axes[i].color.x * 1.2f, axes[i].color.y * 1.2f, axes[i].color.z * 1.2f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, axes[i].color);
				if (ImGui::Button(axes[i].tag, ImVec2(chipWidth, 0.0f)))
				{
					*axes[i].component = resetValue;
					changed = true;
				}
				ImGui::PopStyleColor(3);
				ImGui::SameLine(0.0f, 0.0f);
				ImGui::SetNextItemWidth(fieldWidth);
				const std::string dragId = std::string("##drag") + axes[i].tag;
				changed |= ImGui::DragFloat(dragId.c_str(), axes[i].component, speed);
			}

			ImGui::PopID();
			return changed;
		}
	} // namespace

	const char* EntityDisplayName(const World& world, Entity entity)
	{
		const auto* nc = world.TryGet<NameComponent>(entity);
		return (nc && !nc->name.empty()) ? nc->name.c_str() : "Entity";
	}

	KindBadge EntityKindBadge(const World& world, Entity entity)
	{
		if (world.Has<SkinnedMeshComponent>(entity))
		{
			return {ICON_FA_PERSON_RUNNING, ImVec4(0.55f, 0.75f, 1.00f, 1.0f)};
		}
		if (world.Has<EffectParamsComponent>(entity))
		{
			return {ICON_FA_WAND_MAGIC_SPARKLES, ImVec4(0.80f, 0.55f, 1.00f, 1.0f)};
		}
		if (world.Has<RigidBodyComponent>(entity))
		{
			return {ICON_FA_WEIGHT_HANGING, ImVec4(1.00f, 0.72f, 0.35f, 1.0f)};
		}
		if (world.Has<MeshComponent>(entity))
		{
			return {ICON_FA_CUBE, ImVec4(0.62f, 0.88f, 0.62f, 1.0f)};
		}
		return {ICON_FA_CIRCLE, ImVec4(0.50f, 0.50f, 0.50f, 1.0f)};
	}

	// Remaining section bodies land task-by-task (skinned/hierarchy/tags ->
	// material/effects -> physics/render); each is a guarded no-op until then.

	void DrawTransform(LayerContext& context, World& world, Entity entity)
	{
		auto* tc = world.TryGet<TransformComponent>(entity);
		if (!tc)
		{
			return;
		}
		if (!ImGui::CollapsingHeader(ICON_FA_UP_DOWN_LEFT_RIGHT "  Transform", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		glm::vec3 pos{}, euler{}, scale{};
		DecomposeTRS(tc->localToWorld, pos, euler, scale);

		bool changed = false;
		changed |= DrawVec3Row("Position", pos, 0.0f, 0.05f);
		changed |= DrawVec3Row("Rotation", euler, 0.0f, 0.5f);
		changed |= DrawVec3Row("Scale", scale, 1.0f, 0.02f);
		if (!changed)
		{
			return;
		}

		scale = glm::max(scale, glm::vec3(0.001f)); // zero scale breaks decompose
		tc->localToWorld = ComposeTransform(pos, euler, scale);

		// Children follow the parent verbatim - same rule as das set_transform.
		if (const auto* h = world.TryGet<HierarchyComponent>(entity))
		{
			for (const Entity child: h->children)
			{
				if (auto* childTc = world.TryGet<TransformComponent>(child))
				{
					childTc->localToWorld = tc->localToWorld;
				}
			}
		}

		// True physics teleport: move the Jolt body AND rewrite the interpolation
		// state (prev == curr), otherwise the next sync stomps the edit or the
		// renderer lerps across the jump. Quat order mirrors ComposeTransform (YXZ).
		if (auto* ps = world.TryGet<PhysicsStateComponent>(entity))
		{
			const glm::quat q = glm::angleAxis(glm::radians(euler.y), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(euler.x), glm::vec3(1, 0, 0)) * glm::angleAxis(glm::radians(euler.z), glm::vec3(0, 0, 1));
			ps->prevPosition = pos;
			ps->currPosition = pos;
			ps->prevRotation = q;
			ps->currRotation = q;
			ps->scale = scale;

			const auto* rb = world.TryGet<RigidBodyComponent>(entity);
			auto* physics = context.TryGet<PhysicsSystem>();
			if (rb && physics)
			{
				physics->SetPosition(rb->body, pos);
				physics->SetRotation(rb->body, q);
			}
		}
	}

	void DrawSkinnedMesh(World& world, Entity entity)
	{
		auto* smc = world.TryGet<SkinnedMeshComponent>(entity);
		if (!smc || !ImGui::CollapsingHeader(ICON_FA_FILM "  Skinned Mesh"))
		{
			return;
		}
		int clip = static_cast<int>(smc->clipIndex);
		if (ImGui::InputInt("Clip", &clip))
		{
			smc->clipIndex = static_cast<std::uint32_t>(std::max(0, clip));
		}
		ImGui::DragFloat("Speed", &smc->playbackSpeed, 0.01f, -4.0f, 4.0f);
		ImGui::DragFloat("Time", &smc->animTime, 0.01f, 0.0f, 1000.0f);
		ImGui::Checkbox("Looping", &smc->looping);
		ImGui::TextDisabled("Skin %u, %u joints", smc->skinIndex, smc->jointCount);
	}

	void DrawMaterial(LayerContext& context, World& world, Entity entity)
	{
		(void) context;
		(void) world;
		(void) entity;
	}

	void DrawEffectParams(LayerContext& context, World& world, Entity entity)
	{
		(void) context;
		(void) world;
		(void) entity;
	}

	void DrawPhysics(World& world, Entity entity)
	{
		(void) world;
		(void) entity;
	}

	void DrawMeshPipeline(World& world, Entity entity)
	{
		(void) world;
		(void) entity;
	}

	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection)
	{
		auto* h = world.TryGet<HierarchyComponent>(entity);
		if (!h || !ImGui::CollapsingHeader(ICON_FA_SITEMAP "  Hierarchy"))
		{
			return;
		}

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Parent");
		ImGui::SameLine(86.0f);
		if (h->parent.IsValid())
		{
			const std::string parentLabel = std::string(EntityDisplayName(world, h->parent)) + "  #" + std::to_string(h->parent.id);
			if (ImGui::SmallButton(parentLabel.c_str()))
			{
				selection.Select(h->parent);
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_XMARK " Unparent"))
			{
				ecs::SetParent(world, entity, Entity{});
			}
		}
		else
		{
			ImGui::TextDisabled("(root)");
		}

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Children");
		ImGui::SameLine(86.0f);
		if (h->children.empty())
		{
			ImGui::TextDisabled("(none)");
		}
		else
		{
			ImGui::TextDisabled("%zu", h->children.size());
			for (const Entity child: h->children)
			{
				ImGui::PushID(static_cast<int>(child.id));
				const std::string childLabel = std::string(EntityDisplayName(world, child)) + "  #" + std::to_string(child.id);
				if (ImGui::SmallButton(childLabel.c_str()))
				{
					selection.Select(child);
				}
				ImGui::PopID();
			}
		}
	}

	void DrawTags(World& world, Entity entity, char* addTagBuf, std::size_t addTagBufSize)
	{
		if (!ImGui::CollapsingHeader(ICON_FA_TAG "  Tags"))
		{
			return;
		}

		bool any = false;
		std::uint32_t pendingRemove = UINT32_MAX;
		ForEachTag(
		        [&](const std::string& name, std::uint32_t id)
		        {
			        if (!TagHas(&world, entity.id, id))
			        {
				        return;
			        }
			        any = true;
			        ImGui::PushID(static_cast<int>(id));
			        ImGui::AlignTextToFramePadding();
			        ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.45f, 1.0f), ICON_FA_TAG);
			        ImGui::SameLine();
			        ImGui::TextUnformatted(name.c_str());
			        ImGui::SameLine();
			        if (ImGui::SmallButton(ICON_FA_XMARK))
			        {
				        pendingRemove = id; // defer: don't mutate while enumerating
			        }
			        ImGui::PopID();
		        });
		if (pendingRemove != UINT32_MAX)
		{
			TagRemove(&world, entity.id, pendingRemove);
		}
		if (!any)
		{
			ImGui::TextDisabled("No tags");
		}

		ImGui::SetNextItemWidth(160.0f);
		const bool entered = ImGui::InputTextWithHint("##addTag", "Add tag...", addTagBuf, addTagBufSize, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if ((ImGui::SmallButton(ICON_FA_PLUS " Add") || entered) && addTagBuf[0] != '\0')
		{
			const std::uint32_t id = TagCreate(addTagBuf);
			if (id != UINT32_MAX)
			{
				TagAdd(&world, entity.id, id);
			}
			addTagBuf[0] = '\0';
		}
	}
} // namespace aether::app
