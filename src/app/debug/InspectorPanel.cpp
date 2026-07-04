#include "debug/InspectorPanel.hpp"

#include <string>

#include <imgui.h>

#include "debug/SceneSelection.hpp"
#include "layers/AppLayer.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"

namespace aether::app
{
	bool InspectorPanel::IsAlive(const World& world, Entity entity)
	{
		return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
	}

	std::string InspectorPanel::ComponentSummary(const World& world, Entity entity)
	{
		std::string out;
		auto add = [&](std::string_view name)
		{
			if (!out.empty())
			{
				out += ", ";
			}
			out += name;
		};

		if (world.Has<TransformComponent>(entity))
		{
			add("Transform");
		}
		if (world.Has<MeshComponent>(entity))
		{
			add("Mesh");
		}
		if (world.Has<MaterialComponent>(entity))
		{
			add("Material");
		}
		if (world.Has<PipelineComponent>(entity))
		{
			add("Pipeline");
		}
		if (world.Has<SkinnedMeshComponent>(entity))
		{
			add("Skinned");
		}
		if (world.Has<HierarchyComponent>(entity))
		{
			add("Hierarchy");
		}
		if (world.Has<RigidBodyComponent>(entity))
		{
			add("RigidBody");
		}
		if (world.Has<PhysicsStateComponent>(entity))
		{
			add("Physics");
		}
		if (world.Has<PhysicsDebugShapeComponent>(entity))
		{
			add("DebugShape");
		}

		return out.empty() ? "Entity" : out;
	}

	void InspectorPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Inspector");
		{
			World& world = context.Get<World>();
			const Entity selected = context.Get<SceneSelection>().Primary();
			if (!IsAlive(world, selected))
			{
				ImGui::TextDisabled("No selection");
			}
			else
			{
				ImGui::Text("#%u", selected.id);
				ImGui::Text("Components: %s", ComponentSummary(world, selected).c_str());
				if (const auto transform = world.TryGet<TransformComponent>(selected))
				{
					const glm::vec3 pos = glm::vec3(transform->localToWorld[3]);
					ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
				}
				if (const auto skinned = world.TryGet<SkinnedMeshComponent>(selected))
				{
					ImGui::Text("Animation: clip %u, time %.2f, speed %.2f", skinned->clipIndex, skinned->animTime, skinned->playbackSpeed);
				}
				if (const auto rigid = world.TryGet<RigidBodyComponent>(selected))
				{
					const char* motion = "Dynamic";
					if (rigid->motionType == PhysicsMotionType::Static)
					{
						motion = "Static";
					}
					else if (rigid->motionType == PhysicsMotionType::Kinematic)
					{
						motion = "Kinematic";
					}
					ImGui::Text("Rigid body: %s", motion);
				}
				if (const auto physics = world.TryGet<PhysicsStateComponent>(selected))
				{
					ImGui::Text("Physics pos: %.2f, %.2f, %.2f", physics->currPosition.x, physics->currPosition.y, physics->currPosition.z);
					ImGui::Text("Physics scale: %.2f, %.2f, %.2f", physics->scale.x, physics->scale.y, physics->scale.z);
				}
			}
		}
		ImGui::End();
	}
} // namespace aether::app
