#include "debug/InspectorPanel.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "layers/AppLayer.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"
#include "utils/Profiler.hpp"
#include "utils/TomlConfig.hpp"

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
		if (world.Has<SpawnedEntitiesComponent>(entity))
		{
			add("Spawned");
		}
		if (world.Has<ParentEntityComponent>(entity))
		{
			add("Child");
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

	std::string InspectorPanel::SceneEntityLabel(const World& world, Entity entity)
	{
		return std::format("#{}  {}", entity.id, ComponentSummary(world, entity));
	}

	std::vector<Entity> InspectorPanel::CollectSceneEntities(const World& world)
	{
		std::vector<Entity> entities;
		std::unordered_set<std::uint32_t> seen;
		auto addCandidate = [&](Entity entity)
		{
			if (IsAlive(world, entity) && seen.insert(entity.id).second)
			{
				entities.push_back(entity);
			}
		};

		for (const auto& [raw, component]: world.View<TransformComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<MeshComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<MaterialComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<PipelineComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<SkinnedMeshComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<SpawnedEntitiesComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<ParentEntityComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<RigidBodyComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<PhysicsStateComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}
		for (const auto& [raw, component]: world.View<PhysicsDebugShapeComponent>().each())
		{
			(void) component;
			addCandidate(World::FromEntt(raw));
		}

		std::ranges::sort(entities, [](Entity a, Entity b) { return a.id < b.id; });
		return entities;
	}

	void InspectorPanel::OnImGui(LayerContext& context)
	{
		AE_PROFILE_ZONE();

		// Scene window
		ImGui::Begin("Scene");
		{
			World& world = context.Get<World>();
			const auto entities = CollectSceneEntities(world);
			if (!IsAlive(world, m_selectedSceneEntity))
			{
				m_selectedSceneEntity = {};
			}

			ImGui::Text("%zu entities", entities.size());
			ImGui::BeginChild("SceneList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
			const std::size_t count = std::min<std::size_t>(entities.size(), kMaxSceneRows);
			for (std::size_t i = 0; i < count; ++i)
			{
				const Entity entity = entities[i];
				const bool selected = entity == m_selectedSceneEntity;
				if (ImGui::Selectable(SceneEntityLabel(world, entity).c_str(), selected))
				{
					m_selectedSceneEntity = entity;
				}
			}
			if (entities.size() > count)
			{
				ImGui::TextDisabled("Showing first %zu of %zu", count, entities.size());
			}
			ImGui::EndChild();
		}
		ImGui::End();

		// Inspector window
		ImGui::Begin("Inspector");
		{
			World& world = context.Get<World>();
			if (!IsAlive(world, m_selectedSceneEntity))
			{
				ImGui::TextDisabled("No selection");
			}
			else
			{
				ImGui::Text("#%u", m_selectedSceneEntity.id);
				ImGui::Text("Components: %s", ComponentSummary(world, m_selectedSceneEntity).c_str());
				if (const auto transform = world.TryGet<TransformComponent>(m_selectedSceneEntity))
				{
					const glm::vec3 pos = glm::vec3(transform->localToWorld[3]);
					ImGui::Text("Position: %.2f, %.2f, %.2f", pos.x, pos.y, pos.z);
				}
				if (const auto skinned = world.TryGet<SkinnedMeshComponent>(m_selectedSceneEntity))
				{
					ImGui::Text("Animation: clip %u, time %.2f, speed %.2f", skinned->clipIndex, skinned->animTime, skinned->playbackSpeed);
				}
				if (const auto rigid = world.TryGet<RigidBodyComponent>(m_selectedSceneEntity))
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
				if (const auto physics = world.TryGet<PhysicsStateComponent>(m_selectedSceneEntity))
				{
					ImGui::Text("Physics pos: %.2f, %.2f, %.2f", physics->currPosition.x, physics->currPosition.y, physics->currPosition.z);
					ImGui::Text("Physics scale: %.2f, %.2f, %.2f", physics->scale.x, physics->scale.y, physics->scale.z);
				}
			}
		}
		ImGui::End();
	}

	void InspectorPanel::LoadSettings(TomlConfig& /*config*/, LayerContext& /*context*/)
	{
	}

	void InspectorPanel::SaveSettings(TomlConfig& /*config*/, LayerContext& /*context*/) const
	{
	}
} // namespace aether::app
