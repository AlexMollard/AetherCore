#include "debug/ComponentDrawers.hpp"

#include <algorithm>
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
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
		auto* mc = world.TryGet<MaterialComponent>(entity);
		if (!mc || !ImGui::CollapsingHeader(ICON_FA_PALETTE "  Material", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		auto* assets = context.TryGet<AssetManager>();
		if (!assets)
		{
			ImGui::TextDisabled("Asset manager unavailable");
			return;
		}
		MaterialRegistry& registry = assets->GetMaterialRegistry();

		// Copy-on-write: edits go through a per-entity instance seeded from the
		// entity's current registry material, so the fox keeps its textures when
		// a single factor is dragged.
		auto* inst = world.TryGet<MaterialInstanceComponent>(entity);
		if (!inst)
		{
			MaterialAsset seed{};
			if (!registry.TryDescribe(mc->handle, seed))
			{
				ImGui::TextDisabled("Stale material handle (GPU slot %u)", mc->gpuSlot);
				return;
			}
			inst = &world.Emplace<MaterialInstanceComponent>(entity, MaterialInstanceComponent{seed});
		}
		MaterialAsset& asset = inst->asset;

		bool changed = false;
		changed |= ImGui::ColorEdit4("Base color", &asset.baseColorFactor.x);
		changed |= ImGui::SliderFloat("Metallic", &asset.metallicFactor, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Roughness", &asset.roughnessFactor, 0.0f, 1.0f);
		changed |= ImGui::SliderFloat("Occlusion", &asset.occlusionStrength, 0.0f, 1.0f);
		changed |= ImGui::ColorEdit3("Emissive", &asset.emissiveFactor.x);

		changed |= ImGui::Checkbox("Two-sided", &asset.doubleSided);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Blend", &asset.alphaBlend);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Mask", &asset.alphaMask);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Vtx color", &asset.modulateVertexColor);
		if (asset.alphaMask)
		{
			changed |= ImGui::DragFloat("Cutoff", &asset.alphaCutoff, 0.01f, 0.0f, 1.0f);
		}

		if (changed)
		{
			// One dedup-safe reassign per edited frame; flag changes re-resolve the
			// pipeline (two-sided/blend), and effect-driven entities keep theirs.
			MaterialSystem::AssignMaterial(world, entity, registry, assets->GetPipelineCache(), asset);
		}

		// Texture slots are read-only; swapping needs an asset picker (later spec).
		auto textureRow = [](const char* label, TextureHandle h)
		{
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine(120.0f);
			if (h.index == TextureHandle::kBrokenIndex)
			{
				ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.85f, 1.0f), ICON_FA_IMAGE "  missing (magenta fallback)");
			}
			else if (h.IsValid())
			{
				ImGui::Text(ICON_FA_IMAGE "  entry %u", h.index);
			}
			else
			{
				ImGui::TextDisabled("(none)");
			}
		};
		if (ImGui::TreeNodeEx("Textures", ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			textureRow("Albedo", asset.albedoTex);
			textureRow("Normal", asset.normalTex);
			textureRow("Metal/Rough", asset.metallicRoughnessTex);
			textureRow("Occlusion", asset.occlusionTex);
			textureRow("Emissive", asset.emissiveTex);
			ImGui::TreePop();
		}
		ImGui::TextDisabled("GPU slot %u", mc->gpuSlot);
	}

	void DrawEffectParams(LayerContext& context, World& world, Entity entity)
	{
		auto* ep = world.TryGet<EffectParamsComponent>(entity);
		if (!ep || !ImGui::CollapsingHeader(ICON_FA_BOLT "  Effect Params", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		bool changed = false;
		changed |= ImGui::ColorEdit4("Tint", &ep->params.tint.x);
		changed |= ImGui::DragFloat("Speed", &ep->params.speed, 0.02f, 0.0f, 10.0f);
		changed |= ImGui::DragFloat("Scale", &ep->params.scale, 0.02f, 0.0f, 10.0f);
		changed |= ImGui::DragFloat("Intensity", &ep->params.intensity, 0.02f, 0.0f, 10.0f);

		// Same path as the das set_effect_* bindings: mutate the CPU-authoritative
		// copy, one buffer write. GPU reads it next frame.
		if (changed && ep->paramSlot != 0xFFFFFFFFu)
		{
			if (auto* buffer = context.TryGet<EffectParamBuffer>())
			{
				buffer->Write(ep->paramSlot, ep->params);
			}
		}
		ImGui::TextDisabled("Param slot %u", ep->paramSlot);
	}

	void DrawPhysics(World& world, Entity entity)
	{
		const auto* rb = world.TryGet<RigidBodyComponent>(entity);
		const auto* ps = world.TryGet<PhysicsStateComponent>(entity);
		if ((!rb && !ps) || !ImGui::CollapsingHeader(ICON_FA_WEIGHT_HANGING "  Physics"))
		{
			return;
		}
		if (rb)
		{
			const char* motion = rb->motionType == PhysicsMotionType::Static ? "Static" : rb->motionType == PhysicsMotionType::Kinematic ? "Kinematic" : "Dynamic";
			ImGui::Text("Motion: %s", motion);
			ImGui::TextDisabled("(motion/shape changes need a body rebuild - later spec)");
		}
		if (ps)
		{
			ImGui::Text("Position  %.2f  %.2f  %.2f", ps->currPosition.x, ps->currPosition.y, ps->currPosition.z);
			ImGui::Text("Scale     %.2f  %.2f  %.2f", ps->scale.x, ps->scale.y, ps->scale.z);
		}
	}

	void DrawMeshPipeline(World& world, Entity entity)
	{
		const auto* mesh = world.TryGet<MeshComponent>(entity);
		const auto* pipe = world.TryGet<PipelineComponent>(entity);
		if ((!mesh && !pipe) || !ImGui::CollapsingHeader(ICON_FA_GEARS "  Render"))
		{
			return;
		}
		ImGui::Text("Mesh: %s", (mesh && mesh->mesh) ? "present" : "none");
		ImGui::Text("Pipeline: %s", (pipe && pipe->pipeline) ? "present" : "none");
		ImGui::TextDisabled("Asset swapping needs a picker - later spec");
	}

	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection)
	{
		auto* h = world.TryGet<HierarchyComponent>(entity);
		if (!h)
		{
			return;
		}
		const bool open = ImGui::CollapsingHeader(ICON_FA_SITEMAP "  Hierarchy");
		// Removable only when the link is inert - removing a live link would
		// orphan parent/children bookkeeping.
		if (!h->parent.IsValid() && h->children.empty())
		{
			ImGui::SameLine();
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 22.0f);
			if (ImGui::SmallButton(ICON_FA_XMARK "##removeHierarchy"))
			{
				world.Remove<HierarchyComponent>(entity);
				return;
			}
		}
		if (!open)
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
