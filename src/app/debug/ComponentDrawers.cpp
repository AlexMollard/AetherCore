#include "debug/ComponentDrawers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "assets/AssetTypes.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/EditorDragDrop.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "debug/Icons.hpp"
#include "utils/Logger.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "debug/SpriteAuthoringUi.hpp"
#include "layers/AppLayer.hpp"
#include "scripting/CSharpScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "material/EffectParamBuffer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "physics2d/SpriteColliderGen.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/LightComponents.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

namespace aether::editor
{
	using iw::AccentButton;
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
		inline bool DrawVec3Row(const char* label, glm::vec3& value, float resetValue, float speed)
		{
			return iw::Vec3Row(label, value, resetValue, speed);
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

	const char* EntityDisplayName(const World& world, Entity entity)
	{
		const auto* nc = world.TryGet<NameComponent>(entity);
		return (nc && !nc->name.empty()) ? nc->name.c_str() : "Entity";
	}

	KindBadge EntityKindBadge(const World& world, Entity entity)
	{
		using iw::ToImVec4;
		if (world.Has<ui::UICanvas>(entity))
		{
			return {ICON_FA_WINDOW_MAXIMIZE, ToImVec4(colors::Info)};
		}
		// Widgets before the UIImage/UIRect catch-all: they carry a UIRect but should read as
		// their own kind, not a generic image. Icons match the reflected component icons.
		if (world.Has<ui::UISlider>(entity))
		{
			return {ICON_FA_SLIDERS, ToImVec4(colors::Info)};
		}
		if (world.Has<ui::UIToggle>(entity))
		{
			return {ICON_FA_TOGGLE_ON, ToImVec4(colors::Info)};
		}
		if (world.Has<ui::UIButton>(entity))
		{
			return {ICON_FA_SQUARE, ToImVec4(colors::Info)};
		}
		if (world.Has<ui::UIProgressBar>(entity))
		{
			return {ICON_FA_BARS_PROGRESS, ToImVec4(colors::Info)};
		}
		if (world.Has<ui::UIText>(entity))
		{
			return {ICON_FA_FONT, ToImVec4(colors::Info)};
		}
		if (world.Has<ui::UIImage>(entity) || world.Has<ui::UIRect>(entity))
		{
			return {ICON_FA_IMAGE, ToImVec4(colors::Info)};
		}
		if (world.Has<CameraComponent>(entity))
		{
			return {ICON_FA_VIDEO, ToImVec4(colors::Info)};
		}
		if (world.Has<PointLightComponent>(entity) || world.Has<SpotLightComponent>(entity))
		{
			return {ICON_FA_LIGHTBULB, ToImVec4(colors::Yellow)};
		}
		if (world.Has<SkinnedMeshComponent>(entity))
		{
			return {ICON_FA_PERSON_RUNNING, ToImVec4(colors::Info)};
		}
		if (world.Has<EffectParamsComponent>(entity))
		{
			return {ICON_FA_WAND_MAGIC_SPARKLES, ToImVec4(colors::Mauve)};
		}
		if (world.Has<RigidBodyComponent>(entity))
		{
			return {ICON_FA_WEIGHT_HANGING, ToImVec4(colors::Orange)};
		}
		if (world.Has<SpriteRendererComponent>(entity))
		{
			return {ICON_FA_IMAGE, ToImVec4(colors::Orange)};
		}
		if (world.Has<MeshComponent>(entity))
		{
			return {ICON_FA_CUBE, ToImVec4(colors::Success)};
		}
		return {ICON_FA_CIRCLE, ToImVec4(colors::Neutral)};
	}

	void ApplyWorldTransform(app::LayerContext& context, World& world, Entity entity, const glm::mat4& localToWorld)
	{
		if (world.TryGet<TransformComponent>(entity) == nullptr)
		{
			return;
		}
		ecs::SetWorldTransform(world, entity, localToWorld);

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

		auto* physics2D = context.TryGet<Physics2DSystem>();
		for (const Entity e: subtree)
		{
			auto* ps2d = world.TryGet<Physics2DStateComponent>(e);
			const auto* etc = world.TryGet<TransformComponent>(e);
			if (ps2d == nullptr || etc == nullptr)
			{
				continue;
			}
			glm::vec3 pos{}, euler{}, scale{};
			DecomposeTRS(etc->localToWorld, pos, euler, scale);
			ps2d->prevPosition = ps2d->currPosition = glm::vec2(pos);
			ps2d->prevAngle = ps2d->currAngle = glm::radians(euler.z);
			ps2d->depthZ = pos.z;
			ps2d->scale = glm::max(scale, glm::vec3(0.001f));

			const auto* rb2d = world.TryGet<RigidBody2DComponent>(e);
			if (rb2d && physics2D)
			{
				physics2D->SetBodyPosition(rb2d->body, glm::vec2(pos));
				physics2D->SetBodyAngle(rb2d->body, glm::radians(euler.z));
				// A sleeping body teleported in midair would otherwise hang
				// there until something collides with it.
				physics2D->SetBodyAwake(rb2d->body, true);
			}
		}
	}

	void DrawTransform(app::LayerContext& context, World& world, Entity entity)
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
		const bool sprite2D = world.Has<SpriteRendererComponent>(entity);
		changed |= DrawVec3Row("Position", pos, 0.0f, 0.05f);
		if (sprite2D)
		{
			const bool hadOutOfPlaneTransform = std::abs(euler.x) > 0.0001f || std::abs(euler.y) > 0.0001f || std::abs(scale.z - 1.0f) > 0.0001f;
			euler.x = 0.0f;
			euler.y = 0.0f;
			scale.z = 1.0f;
			changed |= hadOutOfPlaneTransform;
			changed |= PropFloat("Rotation Z", &euler.z, 0.5f);
			changed |= PropDrag2("Scale XY", &scale.x, 0.02f);
		}
		else
		{
			changed |= DrawVec3Row("Rotation", euler, 0.0f, 0.5f);
			changed |= DrawVec3Row("Scale", scale, 1.0f, 0.02f);
		}
		if (!changed)
		{
			return;
		}

		scale = glm::max(scale, glm::vec3(0.001f));
		ApplyWorldTransform(context, world, entity, ComposeTransform(pos, euler, scale));

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

		int projection = cam->projection == CameraProjection::Orthographic ? 1 : 0;
		constexpr const char* kProjections[] = {"Perspective", "Orthographic"};
		if (PropCombo("Projection", &projection, kProjections, IM_ARRAYSIZE(kProjections)))
		{
			cam->projection = projection == 1 ? CameraProjection::Orthographic : CameraProjection::Perspective;
		}
		if (cam->projection == CameraProjection::Orthographic)
		{
			PropFloat("Size", &cam->orthographicHeight, 0.1f, 0.01f, 100000.0f, "%.2f");
			cam->orthographicHeight = std::max(0.001f, cam->orthographicHeight);
		}
		else
		{
			PropFloat("FOV", &cam->fovDegrees, 0.2f, 10.0f, 170.0f, "%.1f\xc2\xb0");
			cam->fovDegrees = std::clamp(cam->fovDegrees, 1.0f, 179.0f);
		}
		PropFloat("Near", &cam->nearPlane, 0.01f, 0.001f, 100.0f, "%.3f");
		PropFloat("Far", &cam->farPlane, 1.0f, 0.1f, 100000.0f, "%.1f");
		cam->nearPlane = std::max(0.001f, cam->nearPlane);
		cam->farPlane = std::max(cam->nearPlane + 0.01f, cam->farPlane);

		// Unity's "Clear Flags": the main camera owns the scene background -
		// a flat solid colour, a multi-stop gradient, or the procedural sky.
		ImGui::SeparatorText("Background");
		int background = static_cast<int>(cam->background);
		constexpr const char* kBackgrounds[] = {"Solid Color", "Gradient", "Sky Gradient"};
		if (PropCombo("Clear", &background, kBackgrounds, IM_ARRAYSIZE(kBackgrounds)))
		{
			cam->background = static_cast<CameraBackground>(background);
			if (cam->background == CameraBackground::Gradient && cam->gradientStops.size() < 2)
			{
				cam->gradientStops = CameraComponent{}.gradientStops;
			}
		}
		if (cam->background == CameraBackground::SolidColour)
		{
			PropColor3("Color", &cam->clearColor.x);
		}
		else if (cam->background == CameraBackground::Gradient)
		{
			PropFloat("Angle", &cam->gradientAngleDegrees, 1.0f, -360.0f, 360.0f, "%.0f\xc2\xb0");
			int removeIdx = -1;
			for (int i = 0; i < static_cast<int>(cam->gradientStops.size()); ++i)
			{
				ImGui::PushID(i);
				auto& stop = cam->gradientStops[static_cast<std::size_t>(i)];
				ImGui::ColorEdit3("##col", &stop.colour.x, ImGuiColorEditFlags_NoInputs);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(120.0f);
				ImGui::SliderFloat("##pos", &stop.position, 0.0f, 1.0f, "%.2f");
				stop.position = std::clamp(stop.position, 0.0f, 1.0f);
				if (cam->gradientStops.size() > 2)
				{
					ImGui::SameLine();
					if (ImGui::SmallButton(ICON_FA_XMARK))
					{
						removeIdx = i;
					}
				}
				ImGui::PopID();
			}
			if (removeIdx >= 0)
			{
				cam->gradientStops.erase(cam->gradientStops.begin() + removeIdx);
			}
			constexpr std::size_t kMaxStops = 8;
			if (cam->gradientStops.size() < kMaxStops && ImGui::SmallButton(ICON_FA_PLUS " Add stop"))
			{
				cam->gradientStops.push_back(GradientStop{{1.0f, 1.0f, 1.0f}, 1.0f});
			}
			std::sort(cam->gradientStops.begin(), cam->gradientStops.end(), [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
		}
		if (!isMain)
		{
			ImGui::TextDisabled("Applies while this is the main camera");
		}

		if (auto* orbit = world.TryGet<OrbitCameraComponent>(entity))
		{
			ImGui::SeparatorText("Orbit");
			DrawVec3Row("Target", orbit->target, 0.0f, 0.05f);
			PropFloat("Yaw", &orbit->yaw, 0.5f, -3600.0f, 3600.0f, "%.1f\xc2\xb0");
			PropFloat("Pitch", &orbit->pitch, 0.5f, -ecs::kOrbitPitchLimit, ecs::kOrbitPitchLimit, "%.1f\xc2\xb0");
			orbit->pitch = std::clamp(orbit->pitch, -ecs::kOrbitPitchLimit, ecs::kOrbitPitchLimit);
			PropFloat("Distance", &orbit->distance, 0.1f, 0.1f, 10000.0f, "%.2f");
			orbit->distance = std::max(0.1f, orbit->distance);
			ImGui::TextDisabled("Pose driven by orbit params (target + yaw/pitch/distance)");
		}
		else
		{
			ImGui::TextDisabled("Views along the entity's -Z: rotate to aim");
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
	void DrawHierarchy(World& world, Entity entity, SceneSelection& selection)
	{
		auto* h = world.TryGet<HierarchyComponent>(entity);
		if (!h)
		{
			return;
		}
		const bool open = SectionHeader(ICON_FA_SITEMAP "  Hierarchy", ImGuiTreeNodeFlags_AllowOverlap);
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
			        ImGui::TextColored(iw::ToImVec4(colors::Yellow), ICON_FA_TAG);
			        ImGui::SameLine();
			        ImGui::TextUnformatted(name.c_str());
			        ImGui::SameLine();
			        if (ImGui::SmallButton(ICON_FA_XMARK))
			        {
				        pendingRemove = id;
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
} // namespace aether::editor
