#include "debug/ComponentDrawers.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::app
{
	// Shared inspector toolkit (Color-token property rows, section headers,
	// vector rows, accent/danger buttons). Kept as short aliases so the drawers
	// read cleanly and every component gets the same consistent styling.
	using iw::AccentButton;
	using iw::DangerButton;
	using iw::PropCheckbox;
	using iw::PropColor3;
	using iw::PropColor4;
	using iw::PropCombo;
	using iw::PropComboStr;
	using iw::PropDrag2;
	using iw::PropFloat;
	using iw::PropInputText;
	using iw::PropInt;
	using iw::PropSlider;
	using iw::PropText;
	using iw::RemovableSection;
	using iw::SectionHeader;

	namespace
	{
		// Back-compat name for the shared axis-chip vector row.
		inline bool DrawVec3Row(const char* label, glm::vec3& value, float resetValue, float speed)
		{
			return iw::Vec3Row(label, value, resetValue, speed);
		}

		void ApplyAnchorPreset(ui::UIRect& rect, int preset)
		{
			if (preset == 9)
			{
				rect.anchorMin = {0.f, 0.f};
				rect.anchorMax = {1.f, 1.f};
				rect.offsetMin = {0.f, 0.f};
				rect.offsetMax = {0.f, 0.f};
				return;
			}

			const int xIndex = preset % 3;
			const int yIndex = preset / 3;
			const glm::vec2 anchor{static_cast<float>(xIndex) * 0.5f, static_cast<float>(yIndex) * 0.5f};
			glm::vec2 size = rect.resolvedRect.z > 0.f && rect.resolvedRect.w > 0.f ? glm::vec2(rect.resolvedRect.z, rect.resolvedRect.w) : glm::abs(rect.offsetMax - rect.offsetMin);
			size = glm::max(size, glm::vec2(1.f));

			rect.anchorMin = anchor;
			rect.anchorMax = anchor;
			rect.offsetMin = -rect.pivot * size;
			rect.offsetMax = (glm::vec2(1.f) - rect.pivot) * size;
		}

	} // namespace

	bool HasSelectedAncestor(const World& world, Entity e, const SceneSelection& selection)
	{
		const auto* h = world.TryGet<HierarchyComponent>(e);
		Entity parent = (h != nullptr) ? h->parent : Entity{};
		while (parent.IsValid())
		{
			if (selection.Contains(parent))
			{
				return true;
			}
			const auto* ph = world.TryGet<HierarchyComponent>(parent);
			parent = (ph != nullptr) ? ph->parent : Entity{};
		}
		return false;
	}

	void AssignScriptType(ScriptEntry& script, std::string typeName)
	{
		if (script.path == typeName)
		{
			return;
		}
		script.path = std::move(typeName);
		script.attached = false;
		script.properties.clear();
	}

	ScriptEntry& AddScriptSlot(ScriptComponent& sc, std::string typeName = {})
	{
		ScriptEntry& script = sc.scripts.emplace_back();
		if (!typeName.empty())
		{
			AssignScriptType(script, std::move(typeName));
		}
		return script;
	}

	bool AcceptScriptDrop(ScriptComponent& sc)
	{
		if (!ImGui::BeginDragDropTarget())
		{
			return false;
		}

		bool assigned = false;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload))
		{
			if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
			{
				const auto* script = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
				AddScriptSlot(sc, script->typeName);
				assigned = true;
			}
		}
		ImGui::EndDragDropTarget();
		return assigned;
	}

	void DrawScriptEntry(LayerContext& context, World& world, Entity entity, ScriptComponent& sc, ScriptEntry& script, std::size_t scriptIndex)
	{
		ImGui::PushID(static_cast<int>(scriptIndex));
		auto* cs = context.TryGet<scripting::CSharpScriptingSubsystem>();
		const bool scriptingAvailable = cs != nullptr && cs->IsAvailable();
		const char* preview = script.path.empty() ? "Drop or choose a script" : script.path.c_str();
		ImGui::SetNextItemWidth(-34.0f);
		if (ImGui::BeginCombo("Script Type", preview))
		{
			if (ImGui::Selectable("None", script.path.empty()))
			{
				AssignScriptType(script, {});
			}
			if (cs != nullptr)
			{
				for (const std::string& typeName: cs->GetScriptTypeNames())
				{
					const bool selected = script.path == typeName;
					if (ImGui::Selectable(typeName.c_str(), selected))
					{
						AssignScriptType(script, typeName);
					}
					if (selected)
					{
						ImGui::SetItemDefaultFocus();
					}
				}
			}
			ImGui::EndCombo();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload))
			{
				if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
				{
					const auto* dropped = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
					AssignScriptType(script, dropped->typeName);
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_XMARK))
		{
			sc.scripts.erase(sc.scripts.begin() + static_cast<std::ptrdiff_t>(scriptIndex));
			ImGui::PopID();
			return;
		}

		if (script.path.empty())
		{
			ImGui::TextDisabled("Drag a C# script here from File Explorer.");
			ImGui::PopID();
			return;
		}

		ImGui::TextDisabled(script.attached ? "Attached (running while playing)" : "Attaches on the next Play tick");
		if (script.attached && ImGui::SmallButton("Re-attach"))
		{
			// Next play tick re-runs OnAttach (handy after editing setup code).
			script.attached = false;
		}

		if (!scriptingAvailable)
		{
			ImGui::PopID();
			return;
		}
		const auto props = cs->GetScriptProperties(script.path);
		if (props.empty())
		{
			ImGui::PopID();
			return;
		}

		// A live instance (while playing) is the source of truth; otherwise the
		// value is the stored override, falling back to the type default.
		std::uint64_t handle = 0;
		if (auto* runner = context.TryGet<ScriptComponentSystem>())
		{
			handle = runner->GetInstanceHandle(entity.id, static_cast<std::uint32_t>(scriptIndex));
		}

		ImGui::SeparatorText("Properties");
		std::vector<int> propertyOrder;
		propertyOrder.reserve(props.size());
		for (int i = 0; i < static_cast<int>(props.size()); ++i)
		{
			if (props[i].name == "Self" && props[i].type == ScriptPropertyValue::Type::Entity)
			{
				propertyOrder.insert(propertyOrder.begin(), i);
			}
			else
			{
				propertyOrder.push_back(i);
			}
		}
		for (const int i: propertyOrder)
		{
			const auto& info = props[i];
			const bool isSelfProperty = info.name == "Self" && info.type == ScriptPropertyValue::Type::Entity;
			ScriptPropertyValue value;
			value.type = info.type;
			bool haveValue = false;
			if (handle != 0 && cs->GetPropertyValue(handle, i, value))
			{
				haveValue = true;
			}
			else if (const auto it = script.properties.find(info.name); it != script.properties.end())
			{
				value = it->second;
				haveValue = true;
			}
			else if (isSelfProperty)
			{
				value.i64 = entity.id;
				haveValue = true;
			}
			else if (cs->GetDefaultPropertyValue(script.path, i, value))
			{
				haveValue = true;
			}
			if (!haveValue)
			{
				continue;
			}

			bool edited = false;
			switch (info.type)
			{
				case ScriptPropertyValue::Type::Float:
				{
					float f = value.f4[0];
					if (PropFloat(info.name.c_str(), &f, 0.1f))
					{
						value.f4[0] = f;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Int:
				case ScriptPropertyValue::Type::Enum:
				{
					int n = static_cast<int>(value.i64);
					if (PropInt(info.name.c_str(), &n))
					{
						value.i64 = n;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Bool:
				{
					bool b = value.i64 != 0;
					if (PropCheckbox(info.name.c_str(), &b))
					{
						value.i64 = b ? 1 : 0;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Vector3:
				{
					glm::vec3 v{value.f4[0], value.f4[1], value.f4[2]};
					if (iw::Vec3Row(info.name.c_str(), v, 0.0f, 0.1f))
					{
						value.f4[0] = v.x;
						value.f4[1] = v.y;
						value.f4[2] = v.z;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::String:
				{
					char buf[256];
					std::snprintf(buf, sizeof(buf), "%s", value.str.c_str());
					if (PropInputText(info.name.c_str(), buf, sizeof(buf)))
					{
						value.str = buf;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::Entity:
				{
					const Entity target{static_cast<std::uint32_t>(value.i64)};
					const bool targetAlive = target.IsValid() && world.GetRegistry().valid(World::ToEntt(target));
					std::string label = targetAlive ? std::string(EntityDisplayName(world, target)) + " #" + std::to_string(target.id) : "None";
					const std::string buttonId = label + "##entityField" + info.name;
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(info.name.c_str());
					ImGui::SameLine(iw::kLabelWidth);
					const ImGuiStyle& style = ImGui::GetStyle();
					const float clearButtonWidth = ImGui::CalcTextSize(ICON_FA_XMARK).x + style.FramePadding.x * 2.0f;
					const float selfButtonWidth = ImGui::CalcTextSize(ICON_FA_LINK).x + style.FramePadding.x * 2.0f;
					const float trailingButtonWidth = clearButtonWidth + style.ItemSpacing.x + (isSelfProperty ? selfButtonWidth + style.ItemSpacing.x : 0.0f);
					const float entityButtonWidth = std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().x - trailingButtonWidth);
					ImGui::Button(buttonId.c_str(), ImVec2(entityButtonWidth, 0.0f));
					if (ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kEntityPayload))
						{
							if (payload->DataSize == sizeof(std::uint32_t))
							{
								value.i64 = *static_cast<const std::uint32_t*>(payload->Data);
								edited = true;
							}
							else if (payload->DataSize == sizeof(dragdrop::EntityPayload))
							{
								value.i64 = static_cast<const dragdrop::EntityPayload*>(payload->Data)->id;
								edited = true;
							}
						}
						ImGui::EndDragDropTarget();
					}
					ImGui::SameLine();
					if (isSelfProperty)
					{
						if (ImGui::SmallButton((std::string(ICON_FA_LINK "##useSelf") + info.name).c_str()))
						{
							value.i64 = entity.id;
							edited = true;
						}
						ImGui::SetItemTooltip("Set Self to this entity");
						ImGui::SameLine();
					}
					if (ImGui::SmallButton((std::string(ICON_FA_XMARK "##clear") + info.name).c_str()))
					{
						value.i64 = 0;
						edited = true;
					}
					break;
				}
				case ScriptPropertyValue::Type::None:
				default:
					break;
			}

			if (edited)
			{
				value.type = info.type;
				script.properties[info.name] = value; // persist the override
				if (handle != 0)
				{
					cs->SetPropertyValue(handle, i, value); // live-apply while playing
				}
			}
		}
		ImGui::PopID();
	}

	const char* EntityDisplayName(const World& world, Entity entity)
	{
		const auto* nc = world.TryGet<NameComponent>(entity);
		return (nc && !nc->name.empty()) ? nc->name.c_str() : "Entity";
	}

	KindBadge EntityKindBadge(const World& world, Entity entity)
	{
		if (world.Has<ui::UICanvas>(entity))
		{
			return {ICON_FA_IMAGE, ImVec4(0.52f, 0.78f, 1.00f, 1.0f)};
		}
		if (world.Has<ui::UIText>(entity))
		{
			return {ICON_FA_CODE, ImVec4(0.75f, 0.90f, 1.00f, 1.0f)};
		}
		if (world.Has<ui::UIImage>(entity) || world.Has<ui::UIRect>(entity))
		{
			return {ICON_FA_IMAGE, ImVec4(0.55f, 0.85f, 0.95f, 1.0f)};
		}
		if (world.Has<CameraComponent>(entity))
		{
			return {ICON_FA_VIDEO, ImVec4(0.60f, 0.80f, 1.00f, 1.0f)};
		}
		if (world.Has<PointLightComponent>(entity) || world.Has<SpotLightComponent>(entity))
		{
			return {ICON_FA_LIGHTBULB, ImVec4(1.00f, 0.86f, 0.40f, 1.0f)};
		}
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

	void ApplyWorldTransform(LayerContext& context, World& world, Entity entity, const glm::mat4& localToWorld)
	{
		if (world.TryGet<TransformComponent>(entity) == nullptr)
		{
			return;
		}
		// Children keep their RELATIVE offsets: the edit's world-space delta
		// cascades through the whole subtree (grandchildren included).
		ecs::SetWorldTransform(world, entity, localToWorld);

		// True physics teleport for every body the edit moved (the entity and
		// any descendant with one): set the Jolt body AND rewrite the
		// interpolation state (prev == curr), otherwise the next sync stomps
		// the edit or the renderer lerps across the jump. Quat order mirrors
		// ComposeTransform (YXZ).
		auto* physics = context.TryGet<PhysicsSystem>();
		std::vector<Entity> subtree{entity};
		for (std::size_t i = 0; i < subtree.size(); ++i)
		{
			if (const auto* h = world.TryGet<HierarchyComponent>(subtree[i]))
			{
				subtree.insert(subtree.end(), h->children.begin(), h->children.end());
			}
		}
		for (const Entity e: subtree)
		{
			auto* ps = world.TryGet<PhysicsStateComponent>(e);
			const auto* etc = world.TryGet<TransformComponent>(e);
			if (ps == nullptr || etc == nullptr)
			{
				continue;
			}
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(etc->localToWorld, pos, euler, scale);
			const glm::quat q = glm::angleAxis(glm::radians(euler.y), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(euler.x), glm::vec3(1, 0, 0)) * glm::angleAxis(glm::radians(euler.z), glm::vec3(0, 0, 1));
			ps->prevPosition = pos;
			ps->currPosition = pos;
			ps->prevRotation = q;
			ps->currRotation = q;
			ps->scale = glm::max(scale, glm::vec3(0.001f));

			const auto* rb = world.TryGet<RigidBodyComponent>(e);
			if (rb && physics)
			{
				physics->SetPosition(rb->body, pos);
				physics->SetRotation(rb->body, q);
			}
		}
	}

	void DrawTransform(LayerContext& context, World& world, Entity entity)
	{
		auto* tc = world.TryGet<TransformComponent>(entity);
		if (!tc)
		{
			return;
		}
		if (!SectionHeader(ICON_FA_UP_DOWN_LEFT_RIGHT "  Transform", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		glm::vec3 pos{}, euler{}, scale{};
		DecomposeTRS(tc->localToWorld, pos, euler, scale);
		const glm::vec3 pos0 = pos;
		const glm::vec3 euler0 = euler;
		const glm::vec3 scale0 = scale;

		bool changed = false;
		changed |= DrawVec3Row("Position", pos, 0.0f, 0.05f);
		changed |= DrawVec3Row("Rotation", euler, 0.0f, 0.5f);
		changed |= DrawVec3Row("Scale", scale, 1.0f, 0.02f);
		if (!changed)
		{
			return;
		}

		scale = glm::max(scale, glm::vec3(0.001f)); // zero scale breaks decompose
		ApplyWorldTransform(context, world, entity, ComposeTransform(pos, euler, scale));

		// Multi-select: propagate this edit as a per-channel delta to the rest of
		// the selection so a drag moves/rotates/scales the whole group together,
		// each entity keeping its own pose. Entities already carried by a selected
		// ancestor's cascade are skipped so they are not moved twice.
		const auto* selection = context.TryGet<SceneSelection>();
		if (selection != nullptr && selection->All().size() > 1)
		{
			const TransformDelta delta{pos - pos0, euler - euler0, scale - scale0};
			for (const Entity other: selection->All())
			{
				if (other == entity || HasSelectedAncestor(world, other, *selection))
				{
					continue;
				}
				if (const auto* otc = world.TryGet<TransformComponent>(other))
				{
					ApplyWorldTransform(context, world, other, ApplyTransformDelta(otc->localToWorld, delta));
				}
			}
		}
	}

	void DrawSkinnedMesh(World& world, Entity entity)
	{
		auto* smc = world.TryGet<SkinnedMeshComponent>(entity);
		if (!smc || !SectionHeader(ICON_FA_FILM "  Skinned Mesh"))
		{
			return;
		}

		// A multi-mesh model spawns one entity per primitive, each with its
		// own SkinnedMeshComponent - editing just the selected part desyncs
		// the model (parts on different clips/phases). Edits drive the whole
		// group: the part's parent (its model root) scopes the subtree, and
		// the shared animation database filters out other skinned models that
		// happen to sit under the same root. Mirrors das set_animation.
		std::vector<SkinnedMeshComponent*> group;
		{
			Entity groupRoot = entity;
			if (const auto* h = world.TryGet<HierarchyComponent>(entity); h && h->parent.IsValid())
			{
				groupRoot = h->parent;
			}
			std::vector<Entity> subtree{groupRoot};
			for (std::size_t i = 0; i < subtree.size(); ++i)
			{
				if (const auto* h = world.TryGet<HierarchyComponent>(subtree[i]))
				{
					subtree.insert(subtree.end(), h->children.begin(), h->children.end());
				}
			}
			for (const Entity e: subtree)
			{
				if (auto* part = world.TryGet<SkinnedMeshComponent>(e); part && part->animDb == smc->animDb)
				{
					group.push_back(part);
				}
			}
		}

		int clip = static_cast<int>(smc->clipIndex);
		if (PropInt("Clip", &clip, 0.1f, 0, 0, "Animation clip index"))
		{
			const auto clipIndex = static_cast<std::uint32_t>(std::max(0, clip));
			for (auto* part: group)
			{
				part->clipIndex = clipIndex;
				part->animTime = 0.0f; // restart together so parts stay in phase
			}
		}
		if (PropFloat("Speed", &smc->playbackSpeed, 0.01f, -4.0f, 4.0f, "%.2f", "Playback rate (negative reverses)"))
		{
			for (auto* part: group)
			{
				part->playbackSpeed = smc->playbackSpeed;
			}
		}
		if (PropFloat("Time", &smc->animTime, 0.01f, 0.0f, 1000.0f, "%.2f", "Current time along the clip"))
		{
			for (auto* part: group)
			{
				part->animTime = smc->animTime;
			}
		}
		if (PropCheckbox("Looping", &smc->looping))
		{
			for (auto* part: group)
			{
				part->looping = smc->looping;
			}
		}
		PropText("Skin", "%u  (%u joints)", smc->skinIndex, smc->jointCount);
		if (group.size() > 1)
		{
			ImGui::TextDisabled("Drives all %zu skinned parts of this model", group.size());
		}
	}

	void DrawMaterial(LayerContext& context, World& world, Entity entity)
	{
		auto* mc = world.TryGet<MaterialComponent>(entity);
		if (!mc || !SectionHeader(ICON_FA_PALETTE "  Material", ImGuiTreeNodeFlags_DefaultOpen))
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
		std::vector<TextureHandle> transientTextureRefs;

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
		changed |= PropColor4("Base color", &asset.baseColorFactor.x);
		changed |= PropSlider("Metallic", &asset.metallicFactor, 0.0f, 1.0f, "%.2f");
		changed |= PropSlider("Roughness", &asset.roughnessFactor, 0.0f, 1.0f, "%.2f");
		changed |= PropSlider("Occlusion", &asset.occlusionStrength, 0.0f, 1.0f, "%.2f");
		changed |= PropColor3("Emissive", &asset.emissiveFactor.x);

		iw::PropLabel("Flags");
		changed |= ImGui::Checkbox("Two-sided", &asset.doubleSided);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Blend", &asset.alphaBlend);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Mask", &asset.alphaMask);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Vtx color", &asset.modulateVertexColor);
		if (asset.alphaMask)
		{
			changed |= PropFloat("Cutoff", &asset.alphaCutoff, 0.01f, 0.0f, 1.0f, "%.2f");
		}

		auto textureRow = [&](const char* label, TextureHandle& h)
		{
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine(iw::kLabelWidth);
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
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload))
				{
					if (payload->DataSize == sizeof(dragdrop::FilePayload))
					{
						const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
						if (file->kind == dragdrop::FileKind::Texture)
						{
							h = assets->GetTextureRegistry().Acquire(file->path);
							transientTextureRefs.push_back(h);
							changed = true;
						}
					}
				}
				ImGui::EndDragDropTarget();
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
		if (changed)
		{
			// One dedup-safe reassign per edited frame; flag changes re-resolve the
			// pipeline (two-sided/blend), and effect-driven entities keep theirs.
			MaterialSystem::AssignMaterial(world, entity, registry, assets->GetPipelineCache(), asset);
			for (TextureHandle h: transientTextureRefs)
			{
				if (h.IsValid())
				{
					assets->GetTextureRegistry().Release(h);
				}
			}
		}
		PropText("GPU slot", "%u", mc->gpuSlot);
	}

	void DrawUiCanvas(World& world, Entity entity)
	{
		auto* canvas = world.TryGet<ui::UICanvas>(entity);
		if (canvas == nullptr || !SectionHeader(ICON_FA_IMAGE "  UI Canvas", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		int scaleMode = static_cast<int>(canvas->scaleMode);
		if (PropComboStr("Scale mode", &scaleMode, "Constant Pixel\0Scale With Reference\0"))
		{
			canvas->scaleMode = static_cast<ui::UICanvas::ScaleMode>(std::clamp(scaleMode, 0, 1));
		}
		PropDrag2("Reference res", &canvas->referenceResolution.x, 1.f, 1.f, 16384.f, "%.0f");
		canvas->referenceResolution = glm::max(canvas->referenceResolution, glm::vec2(1.f));
		PropInt("Sort bias", &canvas->sortBias, 1.f, -100000, 100000);
	}

	void DrawUiRect(World& world, Entity entity)
	{
		auto* rect = world.TryGet<ui::UIRect>(entity);
		if (rect == nullptr || !SectionHeader(ICON_FA_EXPAND "  UI Rect", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		static constexpr const char* kPresets[] = {
		        "Top-Left",
		        "Top-Center",
		        "Top-Right",
		        "Middle-Left",
		        "Center",
		        "Middle-Right",
		        "Bottom-Left",
		        "Bottom-Center",
		        "Bottom-Right",
		        "Stretch-All",
		};

		int preset = -1;
		if (PropCombo("Anchor preset", &preset, kPresets, IM_ARRAYSIZE(kPresets)) && preset >= 0)
		{
			ApplyAnchorPreset(*rect, preset);
		}

		PropDrag2("Anchor min", &rect->anchorMin.x, 0.01f, 0.f, 1.f, "%.2f");
		PropDrag2("Anchor max", &rect->anchorMax.x, 0.01f, 0.f, 1.f, "%.2f");
		rect->anchorMin = glm::clamp(rect->anchorMin, glm::vec2(0.f), glm::vec2(1.f));
		rect->anchorMax = glm::clamp(rect->anchorMax, glm::vec2(0.f), glm::vec2(1.f));
		rect->anchorMax = glm::max(rect->anchorMax, rect->anchorMin);

		PropDrag2("Offset min", &rect->offsetMin.x, 1.f, 0.f, 0.f, "%.0f");
		PropDrag2("Offset max", &rect->offsetMax.x, 1.f, 0.f, 0.f, "%.0f");
		if (rect->anchorMin == rect->anchorMax)
		{
			rect->offsetMax = glm::max(rect->offsetMax, rect->offsetMin + glm::vec2(1.f));
		}
		PropDrag2("Pivot", &rect->pivot.x, 0.01f, 0.f, 1.f, "%.2f");
		rect->pivot = glm::clamp(rect->pivot, glm::vec2(0.f), glm::vec2(1.f));
		ImGui::TextDisabled("Resolved %.1f, %.1f  %.1f x %.1f", rect->resolvedRect.x, rect->resolvedRect.y, rect->resolvedRect.z, rect->resolvedRect.w);
	}

	void DrawUiImage(World& world, Entity entity)
	{
		auto* image = world.TryGet<ui::UIImage>(entity);
		if (image == nullptr || !SectionHeader(ICON_FA_IMAGE "  UI Image", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		PropColor4("Color", &image->color.x);
		PropFloat("Corner radius", &image->cornerRadius, 0.5f, 0.f, 200.f, "%.1f");
		if (image->texture.IsValid())
		{
			PropText("Texture", "entry %u", image->texture.index);
		}
		else
		{
			PropText("Texture", "(none)");
		}
	}

	void DrawUiText(World& world, Entity entity)
	{
		auto* text = world.TryGet<ui::UIText>(entity);
		if (text == nullptr || !SectionHeader(ICON_FA_CODE "  UI Text", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		char textBuf[512]{};
		std::snprintf(textBuf, sizeof(textBuf), "%s", text->text.c_str());
		iw::PropLabel("Text");
		if (ImGui::InputTextMultiline("##text", textBuf, sizeof(textBuf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f)))
		{
			text->text = textBuf;
		}

		char fontBuf[128]{};
		std::snprintf(fontBuf, sizeof(fontBuf), "%s", text->fontName.c_str());
		if (PropInputText("Font", fontBuf, sizeof(fontBuf), "default"))
		{
			text->fontName = fontBuf;
		}

		PropFloat("Pixel size", &text->pixelSize, 0.5f, 4.f, 200.f, "%.0f");
		text->pixelSize = std::max(text->pixelSize, 1.f);
		PropColor4("Color", &text->color.x);

		int hAlign = static_cast<int>(text->hAlign);
		if (PropComboStr("H align", &hAlign, "Left\0Center\0Right\0"))
		{
			text->hAlign = static_cast<ui::UIText::HAlign>(std::clamp(hAlign, 0, 2));
		}
		int vAlign = static_cast<int>(text->vAlign);
		if (PropComboStr("V align", &vAlign, "Top\0Middle\0Bottom\0"))
		{
			text->vAlign = static_cast<ui::UIText::VAlign>(std::clamp(vAlign, 0, 2));
		}
		PropCheckbox("Wrap", &text->wrap);
	}

	void DrawEffectParams(LayerContext& context, World& world, Entity entity)
	{
		auto* ep = world.TryGet<EffectParamsComponent>(entity);
		if (!ep)
		{
			return;
		}
		bool removeEffect = false;
		const bool open = RemovableSection(ICON_FA_BOLT "  Effect Params", ICON_FA_XMARK "##removeEffect", removeEffect, ImGuiTreeNodeFlags_DefaultOpen);
		if (removeEffect)
		{
			// The param slot frees through the on_destroy hook. The material
			// pipeline the effect was overriding comes back via a plain
			// re-assign (AssignMaterial's effect guard no longer trips once
			// EffectParamsComponent is gone).
			world.Remove<EffectParamsComponent>(entity);
			world.Remove<EffectRefComponent>(entity);
			auto* assets = context.TryGet<AssetManager>();
			const auto* mc = world.TryGet<MaterialComponent>(entity);
			MaterialAsset asset{};
			if (assets && mc && assets->GetMaterialRegistry().TryDescribe(mc->handle, asset))
			{
				MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			}
			else
			{
				world.Remove<PipelineComponent>(entity); // effect-only pipeline: nothing to restore
			}
			return;
		}
		if (!open)
		{
			return;
		}
		if (const auto* er = world.TryGet<EffectRefComponent>(entity))
		{
			ImGui::TextDisabled("Effect '%s'", er->name.c_str());
		}

		bool changed = false;
		changed |= PropColor4("Tint", &ep->params.tint.x);
		changed |= PropFloat("Speed", &ep->params.speed, 0.02f, 0.0f, 10.0f, "%.2f");
		changed |= PropFloat("Scale", &ep->params.scale, 0.02f, 0.0f, 10.0f, "%.2f");
		changed |= PropFloat("Intensity", &ep->params.intensity, 0.02f, 0.0f, 10.0f, "%.2f");

		// Same path as the das set_effect_* bindings: mutate the CPU-authoritative
		// copy, one buffer write. GPU reads it next frame.
		if (changed && ep->paramSlot != 0xFFFFFFFFu)
		{
			if (auto* buffer = context.TryGet<EffectParamBuffer>())
			{
				buffer->Write(ep->paramSlot, ep->params);
			}
		}
		PropText("Param slot", "%u", ep->paramSlot);
	}

	void DrawPhysics(LayerContext& context, World& world, Entity entity)
	{
		auto* collider = world.TryGet<ColliderComponent>(entity);
		auto* rb = world.TryGet<RigidBodyComponent>(entity);
		auto* ps = world.TryGet<PhysicsStateComponent>(entity);
		if (collider == nullptr && rb == nullptr)
		{
			return;
		}

		auto* physics = context.TryGet<PhysicsSystem>();
		bool rebuild = false;

		// ── Collider: shape + dimensions + surface material ────────────────────
		if (collider != nullptr)
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_CUBE "  Collider", ICON_FA_XMARK "##removeCollider", removed, ImGuiTreeNodeFlags_DefaultOpen);
			if (removed)
			{
				// Removing the collider removes the whole body (collider + rigid body).
				if (physics != nullptr)
				{
					physics->RemoveBody(world, entity);
				}
				else
				{
					world.Remove<ColliderComponent>(entity);
				}
				return;
			}
			if (open)
			{
				const char* kShapes[] = {"Box", "Sphere", "Capsule", "Cylinder"};
				int shapeIdx = static_cast<int>(collider->shape);
				if (PropCombo("Shape", &shapeIdx, kShapes, IM_ARRAYSIZE(kShapes)))
				{
					collider->shape = static_cast<PhysicsShapeType>(std::clamp(shapeIdx, 0, 3));
					rebuild = true;
				}
				switch (collider->shape)
				{
					case PhysicsShapeType::Box:
						iw::PropLabel("Half extents");
						ImGui::DragFloat3("##he", &collider->halfExtents.x, 0.02f, 0.01f, 1000.0f);
						rebuild |= ImGui::IsItemDeactivatedAfterEdit();
						break;
					case PhysicsShapeType::Sphere:
						PropFloat("Radius", &collider->radius, 0.02f, 0.01f, 1000.0f, "%.3f");
						rebuild |= ImGui::IsItemDeactivatedAfterEdit();
						break;
					case PhysicsShapeType::Capsule:
					case PhysicsShapeType::Cylinder:
						PropFloat("Radius", &collider->radius, 0.02f, 0.01f, 1000.0f, "%.3f");
						rebuild |= ImGui::IsItemDeactivatedAfterEdit();
						PropFloat("Half height", &collider->halfHeight, 0.02f, 0.01f, 1000.0f, "%.3f");
						rebuild |= ImGui::IsItemDeactivatedAfterEdit();
						break;
				}
				iw::PropLabel("Center");
				ImGui::DragFloat3("##center", &collider->center.x, 0.02f);
				rebuild |= ImGui::IsItemDeactivatedAfterEdit();

				bool live = false;
				live |= PropFloat("Friction", &collider->friction, 0.005f, 0.0f, 2.0f, "%.3f");
				live |= PropFloat("Restitution", &collider->restitution, 0.005f, 0.0f, 1.0f, "%.3f");
				collider->friction = std::max(0.0f, collider->friction);
				collider->restitution = std::clamp(collider->restitution, 0.0f, 1.0f);
				rebuild |= PropCheckbox("Sensor (trigger)", &collider->isSensor, "Reports overlaps but produces no collision response");

				if (live && physics != nullptr && rb != nullptr && rb->body.IsValid())
				{
					physics->SetFriction(rb->body, collider->friction);
					physics->SetRestitution(rb->body, collider->restitution);
				}
			}
		}

		// ── Rigid Body: motion + body-level tunables + runtime controls ────────
		if (rb != nullptr)
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_WEIGHT_HANGING "  Rigid Body", ICON_FA_XMARK "##removeRigidBody", removed, ImGuiTreeNodeFlags_DefaultOpen);
			if (removed)
			{
				// Drop the rigid body; a remaining collider re-bakes as a static body.
				world.Remove<RigidBodyComponent>(entity); // on_destroy hook frees the Jolt body
				world.Remove<PhysicsStateComponent>(entity);
				return;
			}
			if (open)
			{
				const char* kMotions[] = {"Static", "Kinematic", "Dynamic"};
				int motionIdx = static_cast<int>(rb->motionType);
				if (PropCombo("Motion", &motionIdx, kMotions, IM_ARRAYSIZE(kMotions)))
				{
					rb->motionType = static_cast<PhysicsMotionType>(std::clamp(motionIdx, 0, 2));
					rebuild = true;
				}

				bool live = false;
				live |= PropFloat("Gravity factor", &rb->gravityFactor, 0.01f, -4.0f, 4.0f, "%.2f");
				PropFloat("Mass", &rb->mass, 0.05f, 0.0f, 100000.0f, "%.2f", "0 = auto (computed from the shape)");
				rebuild |= ImGui::IsItemDeactivatedAfterEdit();
				rb->mass = std::max(0.0f, rb->mass);
				PropFloat("Linear damping", &rb->linearDamping, 0.005f, 0.0f, 1.0f, "%.3f");
				rebuild |= ImGui::IsItemDeactivatedAfterEdit();
				PropFloat("Angular damping", &rb->angularDamping, 0.005f, 0.0f, 1.0f, "%.3f");
				rebuild |= ImGui::IsItemDeactivatedAfterEdit();
				rb->linearDamping = std::clamp(rb->linearDamping, 0.0f, 1.0f);
				rb->angularDamping = std::clamp(rb->angularDamping, 0.0f, 1.0f);
				PropFloat("Max linear vel", &rb->maxLinearVelocity, 1.0f, 0.0f, 100000.0f, "%.0f");
				rebuild |= ImGui::IsItemDeactivatedAfterEdit();
				PropFloat("Max angular vel", &rb->maxAngularVelocity, 0.5f, 0.0f, 10000.0f, "%.1f");
				rebuild |= ImGui::IsItemDeactivatedAfterEdit();

				rebuild |= PropCheckbox("Continuous (CCD)", &rb->continuousCollision, "Continuous collision - stops fast bodies tunneling");
				rebuild |= PropCheckbox("Can sleep", &rb->allowSleeping, "Let the solver deactivate this body when it comes to rest");

				iw::PropLabel("Freeze pos");
				rebuild |= ImGui::Checkbox("X##lockPosX", &rb->lockPosition.x);
				ImGui::SameLine();
				rebuild |= ImGui::Checkbox("Y##lockPosY", &rb->lockPosition.y);
				ImGui::SameLine();
				rebuild |= ImGui::Checkbox("Z##lockPosZ", &rb->lockPosition.z);
				iw::PropLabel("Freeze rot");
				rebuild |= ImGui::Checkbox("X##lockRotX", &rb->lockRotation.x);
				ImGui::SameLine();
				rebuild |= ImGui::Checkbox("Y##lockRotY", &rb->lockRotation.y);
				ImGui::SameLine();
				rebuild |= ImGui::Checkbox("Z##lockRotZ", &rb->lockRotation.z);

				if (live && physics != nullptr && rb->body.IsValid())
				{
					physics->SetGravityFactor(rb->body, rb->gravityFactor);
				}

				if (physics != nullptr && rb->body.IsValid())
				{
					ImGui::SeparatorText("Runtime");
					glm::vec3 velocity = physics->GetLinearVelocity(rb->body);
					if (DrawVec3Row("Velocity", velocity, 0.0f, 0.05f))
					{
						physics->SetLinearVelocity(rb->body, velocity);
					}
					if (ImGui::SmallButton("Impulse +Y"))
					{
						physics->AddImpulse(rb->body, glm::vec3(0.0f, 5.0f, 0.0f));
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("Spin +Y"))
					{
						physics->AddAngularImpulse(rb->body, glm::vec3(0.0f, 2.0f, 0.0f));
					}
					ImGui::SameLine();
					if (ImGui::SmallButton("Stop"))
					{
						physics->SetLinearVelocity(rb->body, glm::vec3(0.0f));
						physics->SetAngularVelocity(rb->body, glm::vec3(0.0f));
					}
					ImGui::SameLine();
					const bool active = physics->IsBodyActive(rb->body);
					if (ImGui::SmallButton(active ? "Sleep" : "Wake"))
					{
						physics->SetBodyActive(rb->body, !active);
					}
					ImGui::SameLine();
					ImGui::TextDisabled(active ? "awake" : "asleep");
				}
			}
		}

		if (ps != nullptr)
		{
			ImGui::TextDisabled("Pos %.2f %.2f %.2f   Scale %.2f %.2f %.2f", ps->currPosition.x, ps->currPosition.y, ps->currPosition.z, ps->scale.x, ps->scale.y, ps->scale.z);
		}

		if (rebuild && physics != nullptr)
		{
			physics->RebuildBody(world, entity);
		}
	}

	void DrawCollisionEvents(World& world, Entity entity)
	{
		auto* ev = world.TryGet<CollisionEventsComponent>(entity);
		if (ev == nullptr)
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_BOLT "  Collision Events", ICON_FA_XMARK "##removeCollisionEvents", removed);
		if (removed)
		{
			world.Remove<CollisionEventsComponent>(entity);
			return;
		}
		if (!open)
		{
			return;
		}
		ImGui::TextDisabled("This frame  contacts +%zu / -%zu   triggers +%zu / -%zu", ev->collisionEnter.size(), ev->collisionExit.size(), ev->triggerEnter.size(), ev->triggerExit.size());
		ImGui::Text("Overlapping (%zu)", ev->overlapping.size());
		int shown = 0;
		for (const Entity other: ev->overlapping)
		{
			if (shown++ >= 12)
			{
				ImGui::BulletText("...");
				break;
			}
			ImGui::BulletText("%s  #%u", EntityDisplayName(world, other), other.id);
		}
		ImGui::TextDisabled("Runtime only - populated while Playing.");
	}

	void DrawJoint(LayerContext& context, World& world, Entity entity)
	{
		auto* joint = world.TryGet<JointComponent>(entity);
		if (joint == nullptr)
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_LINK "  Joint", ICON_FA_XMARK "##removeJoint", removed);
		if (removed)
		{
			world.Remove<JointComponent>(entity);
			return;
		}
		if (!open)
		{
			return;
		}

		auto* physics = context.TryGet<PhysicsSystem>();
		bool rebuild = false;

		const char* kTypes[] = {"Fixed", "Point", "Hinge", "Distance", "Slider"};
		int typeIdx = static_cast<int>(joint->type);
		if (PropCombo("Type", &typeIdx, kTypes, IM_ARRAYSIZE(kTypes)))
		{
			joint->type = static_cast<JointType>(std::clamp(typeIdx, 0, 4));
			rebuild = true;
		}

		// Target body: drag an entity from the hierarchy, or leave empty for world.
		const bool targetAlive = joint->target.IsValid() && world.GetRegistry().valid(World::ToEntt(joint->target));
		const std::string targetLabel = targetAlive ? std::string(EntityDisplayName(world, joint->target)) + "  #" + std::to_string(joint->target.id) : "World (fixed)";
		iw::PropLabel("Target");
		ImGui::Button((targetLabel + "##jointTarget").c_str(), ImVec2(ImGui::GetContentRegionAvail().x - 26.0f, 0.0f));
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kEntityPayload))
			{
				if (payload->DataSize == sizeof(std::uint32_t))
				{
					joint->target = Entity{*static_cast<const std::uint32_t*>(payload->Data)};
					rebuild = true;
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(ICON_FA_XMARK "##clearJointTarget"))
		{
			joint->target = Entity{};
			rebuild = true;
		}

		iw::PropLabel("Anchor");
		ImGui::DragFloat3("##anchor", &joint->anchor.x, 0.02f);
		rebuild |= ImGui::IsItemDeactivatedAfterEdit();
		if (joint->type == JointType::Hinge || joint->type == JointType::Slider)
		{
			iw::PropLabel("Axis");
			ImGui::DragFloat3("##axis", &joint->axis.x, 0.02f);
			rebuild |= ImGui::IsItemDeactivatedAfterEdit();
			PropFloat("Limit min", &joint->minLimit, 0.01f, 0.0f, 0.0f, "%.3f");
			rebuild |= ImGui::IsItemDeactivatedAfterEdit();
			PropFloat("Limit max", &joint->maxLimit, 0.01f, 0.0f, 0.0f, "%.3f");
			rebuild |= ImGui::IsItemDeactivatedAfterEdit();
			ImGui::TextDisabled(joint->type == JointType::Hinge ? "Limits in radians; min>=max = free spin" : "Limits in metres; min>=max = free slide");
		}
		if (joint->type == JointType::Distance)
		{
			PropFloat("Distance", &joint->distance, 0.02f, -1.0f, 10000.0f, "%.3f", "-1 = keep the distance at creation time");
			rebuild |= ImGui::IsItemDeactivatedAfterEdit();
		}
		rebuild |= PropCheckbox("Collide connected", &joint->collideConnected);

		if (rebuild && physics != nullptr)
		{
			physics->RebuildJoint(world, entity);
		}
	}

	void DrawBehaviors(World& world, Entity entity)
	{
		if (auto* bob = world.TryGet<BobComponent>(entity))
		{
			ImGui::PushID(bob);
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_WAVE_SQUARE "  Bob", ICON_FA_XMARK "##removeBob", removed);
			if (removed)
			{
				world.Remove<BobComponent>(entity);
			}
			else if (open)
			{
				PropFloat("Amplitude", &bob->amplitude, 0.02f, 0.0f, 50.0f, "%.2f");
				PropFloat("Frequency", &bob->frequency, 0.01f, 0.0f, 20.0f, "%.2f");
				PropFloat("Phase", &bob->phase, 0.02f, 0.0f, 0.0f, "%.2f");
				if (bob->baseCaptured)
				{
					ImGui::TextDisabled("Base Y %.2f", bob->baseY);
					ImGui::SameLine();
					if (ImGui::SmallButton("Re-base"))
					{
						// Next behavior tick re-captures from the current
						// transform (same as a fresh scene apply).
						bob->baseCaptured = false;
					}
				}
			}
			ImGui::PopID();
		}

		if (auto* spin = world.TryGet<SpinComponent>(entity))
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_ROTATE "  Spin", ICON_FA_XMARK "##removeSpin", removed);
			if (removed)
			{
				world.Remove<SpinComponent>(entity);
			}
			else if (open)
			{
				DrawVec3Row("Deg/sec", spin->eulerDegPerSec, 0.0f, 0.5f);
			}
		}

		if (auto* orbit = world.TryGet<OrbitComponent>(entity))
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_CIRCLE_NOTCH "  Orbit", ICON_FA_XMARK "##removeOrbit", removed);
			if (removed)
			{
				world.Remove<OrbitComponent>(entity);
			}
			else if (open)
			{
				DrawVec3Row("Center", orbit->center, 0.0f, 0.05f);
				PropFloat("Radius", &orbit->radius, 0.05f, 0.0f, 500.0f, "%.2f");
				PropFloat("Speed deg/s", &orbit->angularSpeedDeg, 0.2f, -720.0f, 720.0f, "%.1f");
				PropFloat("Angle", &orbit->angleDeg, 0.5f, 0.0f, 0.0f, "%.1f");
				PropFloat("Yaw offset", &orbit->yawOffsetDeg, 0.5f, 0.0f, 0.0f, "%.1f");
				PropFloat("Height", &orbit->height, 0.05f, 0.0f, 0.0f, "%.2f");
			}
		}

		if (auto* pulse = world.TryGet<MaterialPulseComponent>(entity))
		{
			ImGui::PushID(pulse);
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_HEART_PULSE "  Material Pulse", ICON_FA_XMARK "##removePulse", removed);
			if (removed)
			{
				world.Remove<MaterialPulseComponent>(entity);
			}
			else if (open)
			{
				PropColor3("Emissive A", &pulse->emissiveA.x);
				PropColor3("Emissive B", &pulse->emissiveB.x);
				PropFloat("Frequency", &pulse->frequency, 0.02f, 0.0f, 20.0f, "%.2f");
			}
			ImGui::PopID();
		}

		if (auto* scale = world.TryGet<ScalePulseComponent>(entity))
		{
			ImGui::PushID(scale);
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_EXPAND "  Scale Pulse", ICON_FA_XMARK "##removeScalePulse", removed);
			if (removed)
			{
				world.Remove<ScalePulseComponent>(entity);
			}
			else if (open)
			{
				PropFloat("Amplitude", &scale->amplitude, 0.01f, 0.0f, 4.0f, "%.2f");
				PropFloat("Frequency", &scale->frequency, 0.02f, 0.0f, 20.0f, "%.2f");
				PropFloat("Phase", &scale->phase, 0.02f, 0.0f, 0.0f, "%.2f");
				if (scale->baseCaptured)
				{
					ImGui::TextDisabled("Base %.2f, %.2f, %.2f", scale->baseScale.x, scale->baseScale.y, scale->baseScale.z);
					ImGui::SameLine();
					if (ImGui::SmallButton("Re-base##scalePulse"))
					{
						scale->baseCaptured = false; // next tick re-captures from the transform
					}
				}
			}
			ImGui::PopID();
		}

		if (auto* look = world.TryGet<LookAtComponent>(entity))
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_EYE "  Look At", ICON_FA_XMARK "##removeLookAt", removed);
			if (removed)
			{
				world.Remove<LookAtComponent>(entity);
			}
			else if (open)
			{
				DrawVec3Row("Target", look->target, 0.0f, 0.05f);
				PropCheckbox("Keep upright", &look->keepUpright);
				ImGui::TextDisabled("Aims the entity's -Z at the target while Playing");
			}
		}
	}

	void DrawLights(World& world, Entity entity)
	{
		if (auto* pl = world.TryGet<PointLightComponent>(entity))
		{
			ImGui::PushID("pointLight");
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_LIGHTBULB "  Point Light", ICON_FA_XMARK "##removePointLight", removed, ImGuiTreeNodeFlags_DefaultOpen);
			if (removed)
			{
				world.Remove<PointLightComponent>(entity);
			}
			else if (open)
			{
				PropColor3("Color", &pl->color.x);
				PropFloat("Intensity", &pl->intensity, 0.2f, 0.0f, 1000.0f, "%.1f");
				PropFloat("Radius", &pl->radius, 0.1f, 0.0f, 500.0f, "%.2f");
				PropCheckbox("Casts shadow", &pl->castsShadow);
			}
			ImGui::PopID();
		}

		if (auto* sl = world.TryGet<SpotLightComponent>(entity))
		{
			ImGui::PushID("spotLight");
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_LIGHTBULB "  Spot Light", ICON_FA_XMARK "##removeSpotLight", removed, ImGuiTreeNodeFlags_DefaultOpen);
			if (removed)
			{
				world.Remove<SpotLightComponent>(entity);
			}
			else if (open)
			{
				PropColor3("Color", &sl->color.x);
				PropFloat("Intensity", &sl->intensity, 0.2f, 0.0f, 1000.0f, "%.1f");
				PropFloat("Radius", &sl->radius, 0.1f, 0.0f, 500.0f, "%.2f");
				iw::PropLabel("Inner angle");
				ImGui::SliderAngle("##inner", &sl->innerAngleRad, 1.0f, 89.0f);
				iw::PropLabel("Outer angle");
				ImGui::SliderAngle("##outer", &sl->outerAngleRad, 1.0f, 89.0f);
				if (sl->outerAngleRad < sl->innerAngleRad)
				{
					sl->outerAngleRad = sl->innerAngleRad;
				}
				PropCheckbox("Casts shadow", &sl->castsShadow);
				ImGui::TextDisabled("Aims along the entity's -Z: rotate to aim the cone");
			}
			ImGui::PopID();
		}
	}

	void DrawCamera(World& world, Entity entity)
	{
		auto* cam = world.TryGet<CameraComponent>(entity);
		if (cam == nullptr)
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_VIDEO "  Camera", ICON_FA_XMARK "##removeCamera", removed, ImGuiTreeNodeFlags_DefaultOpen);
		if (removed)
		{
			// CameraSystem reaps the backing pool camera next tick once the
			// component (and its MainCamera tag) are gone.
			world.Remove<CameraComponent>(entity);
			if (world.Has<MainCameraComponent>(entity))
			{
				world.Remove<MainCameraComponent>(entity);
			}
			return;
		}
		if (!open)
		{
			return;
		}

		const bool isMain = world.Has<MainCameraComponent>(entity);
		if (isMain)
		{
			ImGui::TextColored(iw::ToImVec4(colors::Info), ICON_FA_VIDEO "  Main camera");
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear##mainCam"))
			{
				world.Remove<MainCameraComponent>(entity);
			}
			ImGui::SetItemTooltip("Stop using this camera as the scene's main view while Playing");
		}
		else
		{
			if (AccentButton(ICON_FA_VIDEO "  Set as Main Camera", ImVec2(-FLT_MIN, 0.0f)))
			{
				ecs::SetMainCameraEntity(world, entity);
			}
			ImGui::SetItemTooltip("Drive the scene view from this camera while Playing");
		}

		PropFloat("FOV", &cam->fovDegrees, 0.2f, 10.0f, 170.0f, "%.1f\xc2\xb0");
		cam->fovDegrees = std::clamp(cam->fovDegrees, 1.0f, 179.0f);
		PropFloat("Near", &cam->nearPlane, 0.01f, 0.001f, 100.0f, "%.3f");
		PropFloat("Far", &cam->farPlane, 1.0f, 0.1f, 100000.0f, "%.1f");
		cam->nearPlane = std::max(0.001f, cam->nearPlane);
		cam->farPlane = std::max(cam->nearPlane + 0.01f, cam->farPlane);
		ImGui::TextDisabled("Views along the entity's -Z: rotate to aim");
	}

	void AddScriptToEntity(World& world, Entity entity, std::string typeName)
	{
		if (!entity.IsValid() || !world.GetRegistry().valid(World::ToEntt(entity)))
		{
			return;
		}
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			sc = &world.Emplace<ScriptComponent>(entity);
		}
		AddScriptSlot(*sc, std::move(typeName));
	}

	bool AcceptScriptDropOnEntity(World& world, Entity entity)
	{
		if (!ImGui::BeginDragDropTarget())
		{
			return false;
		}

		bool accepted = false;
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload))
		{
			if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
			{
				const auto* script = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
				AddScriptToEntity(world, entity, script->typeName);
				accepted = true;
			}
		}
		ImGui::EndDragDropTarget();
		return accepted;
	}

	void DrawScript(LayerContext& context, World& world, Entity entity)
	{
		auto* sc = world.TryGet<ScriptComponent>(entity);
		if (sc == nullptr)
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_CODE "  Script", ICON_FA_XMARK "##removeScript", removed, ImGuiTreeNodeFlags_DefaultOpen);
		if (removed)
		{
			world.Remove<ScriptComponent>(entity);
			return;
		}
		if (!open)
		{
			return;
		}

		if (ImGui::Button(ICON_FA_PLUS "  Add Script"))
		{
			AddScriptSlot(*sc);
		}
		AcceptScriptDrop(*sc);
		if (sc->scripts.empty())
		{
			ImGui::TextDisabled("Drag a C# script here from File Explorer.");
			return;
		}

		for (std::size_t i = 0; i < sc->scripts.size();)
		{
			ImGui::Separator();
			const std::size_t before = sc->scripts.size();
			DrawScriptEntry(context, world, entity, *sc, sc->scripts[i], i);
			if (sc->scripts.size() == before)
			{
				++i;
			}
		}
	}

	void DrawSceneTransient(World& world, Entity entity)
	{
		if (!world.Has<SceneTransientComponent>(entity))
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_GHOST "  Scene Transient", ICON_FA_XMARK "##removeTransient", removed);
		if (removed)
		{
			world.Remove<SceneTransientComponent>(entity);
			return;
		}
		if (open)
		{
			ImGui::TextWrapped("Excluded from scene capture (saves and Play snapshots), whole subtree included. Script-owned runtime actors carry this so loads don't duplicate them.");
		}
	}

	void DrawMeshPipeline(World& world, Entity entity)
	{
		const auto* mesh = world.TryGet<MeshComponent>(entity);
		const auto* pipe = world.TryGet<PipelineComponent>(entity);
		if ((!mesh && !pipe) || !SectionHeader(ICON_FA_GEARS "  Render"))
		{
			return;
		}
		PropText("Mesh", "%s", (mesh && mesh->mesh) ? "present" : "none");
		PropText("Pipeline", "%s", (pipe && pipe->pipeline) ? "present" : "none");
		ImGui::TextDisabled("Asset swapping needs a picker - later spec");
	}

	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection)
	{
		auto* h = world.TryGet<HierarchyComponent>(entity);
		if (!h)
		{
			return;
		}
		const bool open = SectionHeader(ICON_FA_SITEMAP "  Hierarchy", ImGuiTreeNodeFlags_AllowOverlap);
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
		if (!SectionHeader(ICON_FA_TAG "  Tags"))
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
