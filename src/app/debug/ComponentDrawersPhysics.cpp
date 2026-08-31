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
	void DrawPhysics(app::LayerContext& context, World& world, Entity entity)
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

		// A body needs a shape. PhysicsSystem builds bodies by walking colliders and
		// RebuildBody returns early without one, so a lone Rigid Body simulates nothing - and
		// the scene serialiser captures physics off the collider too, so it is not written to
		// the file either. Every field below would be authored, saved into nothing, and gone
		// on the next load, which is worth saying out loud rather than leaving to be found.
		if (rb != nullptr && collider == nullptr)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, iw::WithAlpha(colors::Orange, 0.95f));
			ImGui::TextWrapped("%s  Add a Collider: without one this body does not simulate and is not saved with the scene.", ICON_FA_TRIANGLE_EXCLAMATION);
			ImGui::PopStyleColor();
			ImGui::Spacing();
		}

		if (collider != nullptr)
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_DRAW_POLYGON "  Collider", ICON_FA_XMARK "##removeCollider", removed, ImGuiTreeNodeFlags_DefaultOpen);
			if (removed)
			{
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
				const char* const kShapes[] = {"Box", "Sphere", "Capsule", "Cylinder"};
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

		if (rb != nullptr)
		{
			bool removed = false;
			const bool open = RemovableSection(ICON_FA_WEIGHT_HANGING "  Rigid Body", ICON_FA_XMARK "##removeRigidBody", removed, ImGuiTreeNodeFlags_DefaultOpen);
			if (removed)
			{
				world.Remove<RigidBodyComponent>(entity);
				world.Remove<PhysicsStateComponent>(entity);
				return;
			}
			if (open)
			{
				const char* const kMotions[] = {"Static", "Kinematic", "Dynamic"};
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

	void DrawCollider2DTools(app::LayerContext& context, World& world, Entity entity)
	{
		auto* collider = world.TryGet<Collider2DComponent>(entity);
		const auto* sprite = world.TryGet<SpriteRendererComponent>(entity);
		if (collider == nullptr || sprite == nullptr)
		{
			return;
		}
		ImGui::PushID("collider2dTools");
		if (ImGui::Button(ICON_FA_BOX_OPEN "  Collider from sprite outline"))
		{
			bool generated = false;
			if (auto* sprites = context.TryGet<SpriteAssetStore>(); sprites != nullptr && !sprite->atlasPath.empty())
			{
				if (const auto atlas = sprites->LoadAtlas(sprite->atlasPath); atlas.has_value())
				{
					const SpriteRegion* region = sprite->spriteId.IsValid() ? (*atlas)->Find(sprite->spriteId) : ((*atlas)->sprites.empty() ? nullptr : &(*atlas)->sprites.front());
					if (region != nullptr && region->collisionOutline.size() >= 3)
					{
						collider->points = BuildColliderPointsFromOutline(region->collisionOutline, region->pixelSize, region->pivot, (*atlas)->pixelsPerUnit);
						collider->shape = Collider2DShape::Polygon;
						if (collider->points.size() > 8)
						{
							AE_WARN(LogCategory::App, "Sprite outline has {} points; Box2D keeps at most 8 after the convex hull", collider->points.size());
						}
						if (auto* physics = context.TryGet<Physics2DSystem>())
						{
							physics->RebuildBody(world, entity);
						}
						generated = true;
					}
				}
			}
			if (!generated)
			{
				AE_WARN(LogCategory::App, "Collider from sprite outline: entity {} has no atlas region with a collision outline (author one in the Sprite Slicer)", entity.id);
			}
		}
		ImGui::SetItemTooltip("Build a polygon collider from the sprite's authored collision outline\n(Sprite Slicer > collision editing; at most 8 points survive the convex hull)");
		ImGui::PopID();
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

	void DrawJoint(app::LayerContext& context, World& world, Entity entity)
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

		const char* const kTypes[] = {"Fixed", "Point", "Hinge", "Distance", "Slider"};
		int typeIdx = static_cast<int>(joint->type);
		if (PropCombo("Type", &typeIdx, kTypes, IM_ARRAYSIZE(kTypes)))
		{
			joint->type = static_cast<JointType>(std::clamp(typeIdx, 0, 4));
			rebuild = true;
		}

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
		const bool clearTarget = ImGui::SmallButton(ICON_FA_XMARK "##clearJointTarget");
		ImGui::SetItemTooltip("Clear the joint target");
		if (clearTarget)
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
} // namespace aether::editor
