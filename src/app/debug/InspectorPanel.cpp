#include "debug/InspectorPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "io/FileSystem.hpp"
#include "material/EffectManager.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
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
			ImGui::SetNextItemWidth(-88.0f);
			if (ImGui::InputText("##name", buf, sizeof(buf)))
			{
				nc->name = buf;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(ICON_FA_XMARK "##removeName"))
			{
				world.Remove<NameComponent>(entity);
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
			// Entity-script list refreshes once per open.
			m_scriptList.clear();
			if (const auto scripts = io::FileSystem::Glob("scripts://entities/*.das"); scripts.has_value())
			{
				m_scriptList = *scripts;
				std::sort(m_scriptList.begin(), m_scriptList.end());
			}
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

			auto* assets = context.TryGet<AssetManager>();
			auto* primitives = context.TryGet<PrimitiveMeshes>();
			auto* sceneCtx = context.TryGet<scripting::SceneContext>();

			// Current pose seeds the smarter defaults (orbit resumes in place,
			// physics shapes match the visual scale).
			glm::vec3 curPos{}, curEuler{}, curScale{1.0f};
			if (const auto* tc = world.TryGet<TransformComponent>(entity))
			{
				DecomposeTRS(tc->localToWorld, curPos, curEuler, curScale);
			}

			ImGui::SeparatorText("Core");
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

			ImGui::SeparatorText("Rendering");
			const bool hasMesh = world.Has<MeshComponent>(entity);
			const auto addPrimitive = [&](PrimitiveMesh kind, const char* label, const char* kindName)
			{
				if (!PaletteEntry(label, m_addFilter, hasMesh) || primitives == nullptr || assets == nullptr)
				{
					return;
				}
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.Emplace<MeshComponent>(entity, MeshComponent{.mesh = &primitives->Get(kind)});
				world.EmplaceOrReplace<MeshSourceComponent>(entity, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = kindName, .primitiveIndex = 0});
				if (!world.Has<MaterialComponent>(entity))
				{
					MaterialAsset asset{};
					asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
					asset.roughnessFactor = 0.6f;
					asset.doubleSided = true;
					MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
				}
			};
			addPrimitive(PrimitiveMesh::Cube, ICON_FA_CUBE "  Mesh - Cube", "cube");
			addPrimitive(PrimitiveMesh::Sphere, ICON_FA_CIRCLE "  Mesh - Sphere", "sphere");
			addPrimitive(PrimitiveMesh::Plane, ICON_FA_IMAGE "  Mesh - Plane", "plane");
			if (PaletteEntry(ICON_FA_PALETTE "  Material", m_addFilter, world.Has<MaterialComponent>(entity)) && assets != nullptr)
			{
				MaterialAsset asset{};
				asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
				asset.roughnessFactor = 0.6f;
				MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			}
			if (PaletteEntry(ICON_FA_LIGHTBULB "  Point Light", m_addFilter, world.Has<PointLightComponent>(entity)))
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.Emplace<PointLightComponent>(entity);
			}
			if (PaletteEntry(ICON_FA_LIGHTBULB "  Spot Light", m_addFilter, world.Has<SpotLightComponent>(entity)))
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.Emplace<SpotLightComponent>(entity);
			}
			if (sceneCtx != nullptr && sceneCtx->effects != nullptr && assets != nullptr)
			{
				const bool hasEffect = world.Has<EffectRefComponent>(entity);
				std::vector<std::string> effectNames;
				sceneCtx->effects->ForEachEffect([&](const std::string& n, const auto&) { effectNames.push_back(n); });
				std::sort(effectNames.begin(), effectNames.end());
				for (const std::string& n: effectNames)
				{
					const std::string label = std::string(ICON_FA_BOLT "  Effect - ") + n;
					if (PaletteEntry(label.c_str(), m_addFilter, hasEffect))
					{
						effects::ApplyEntityEffect(world, entity, n, *sceneCtx->effects, assets->GetPipelineCache(), assets->GetEffectParamBuffer());
					}
				}
			}

			if (!m_scriptList.empty())
			{
				ImGui::SeparatorText("Scripts");
				const bool hasScript = world.Has<ScriptComponent>(entity);
				for (const std::string& fullPath: m_scriptList)
				{
					// Glob yields "scripts://entities/x.das"; the compiler wants
					// the path relative to the scripts root.
					std::string relative = fullPath;
					if (constexpr std::string_view kPrefix = "scripts://"; relative.starts_with(kPrefix))
					{
						relative = relative.substr(kPrefix.size());
					}
					std::string label = relative;
					if (const auto slash = label.find_last_of("/\\"); slash != std::string::npos)
					{
						label = label.substr(slash + 1);
					}
					const std::string entry = std::string(ICON_FA_CODE "  Script - ") + label;
					if (PaletteEntry(entry.c_str(), m_addFilter, hasScript))
					{
						world.Emplace<ScriptComponent>(entity, ScriptComponent{.path = relative});
					}
				}
			}

			ImGui::SeparatorText("Behaviors");
			if (PaletteEntry(ICON_FA_WAVE_SQUARE "  Bob", m_addFilter, world.Has<BobComponent>(entity)))
			{
				world.Emplace<BobComponent>(entity, BobComponent{.amplitude = 1.5f, .frequency = 0.8f});
			}
			if (PaletteEntry(ICON_FA_ROTATE "  Spin", m_addFilter, world.Has<SpinComponent>(entity)))
			{
				world.Emplace<SpinComponent>(entity, SpinComponent{.eulerDegPerSec = {0.0f, 40.0f, 0.0f}});
			}
			if (PaletteEntry(ICON_FA_CIRCLE_NOTCH "  Orbit", m_addFilter, world.Has<OrbitComponent>(entity)))
			{
				// Seed so the first tick resumes exactly where the entity
				// stands: radius/angle derived from its position around origin.
				const float radius = std::max(std::sqrt(curPos.x * curPos.x + curPos.z * curPos.z), 3.0f);
				const float angleDeg = glm::degrees(std::atan2(curPos.z, curPos.x));
				world.Emplace<OrbitComponent>(entity, OrbitComponent{.center = {0.0f, 0.0f, 0.0f}, .radius = radius, .angularSpeedDeg = 30.0f, .angleDeg = angleDeg, .yawOffsetDeg = 0.0f, .height = curPos.y});
			}
			if (PaletteEntry(ICON_FA_HEART_PULSE "  Material Pulse", m_addFilter, world.Has<MaterialPulseComponent>(entity)))
			{
				world.Emplace<MaterialPulseComponent>(entity, MaterialPulseComponent{.emissiveA = {0.0f, 0.0f, 0.05f}, .emissiveB = {0.9f, 0.2f, 0.05f}, .frequency = 2.0f});
			}

			ImGui::SeparatorText("Physics");
			const bool hasBody = world.Has<RigidBodyComponent>(entity) || world.Has<BoxBodyDesc>(entity) || world.Has<SphereBodyDesc>(entity) || world.Has<CapsuleBodyDesc>(entity);
			if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Box Body (dynamic)", m_addFilter, hasBody))
			{
				BoxBodyDesc desc{};
				desc.halfExtents = curScale * 0.5f;
				desc.motionType = PhysicsMotionType::Dynamic;
				world.Emplace<BoxBodyDesc>(entity, desc);
			}
			if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Box Body (static)", m_addFilter, hasBody))
			{
				BoxBodyDesc desc{};
				desc.halfExtents = curScale * 0.5f;
				desc.motionType = PhysicsMotionType::Static;
				world.Emplace<BoxBodyDesc>(entity, desc);
			}
			if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Sphere Body (dynamic)", m_addFilter, hasBody))
			{
				SphereBodyDesc desc{};
				desc.radius = std::max({curScale.x, curScale.y, curScale.z}) * 0.5f;
				desc.motionType = PhysicsMotionType::Dynamic;
				world.Emplace<SphereBodyDesc>(entity, desc);
			}
			if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Capsule Body (dynamic)", m_addFilter, hasBody))
			{
				CapsuleBodyDesc desc{};
				desc.radius = curScale.x * 0.5f;
				desc.halfHeight = curScale.y * 0.5f;
				desc.motionType = PhysicsMotionType::Dynamic;
				world.Emplace<CapsuleBodyDesc>(entity, desc);
			}

			ImGui::SeparatorText("Editor");
			if (PaletteEntry(ICON_FA_GHOST "  Scene Transient", m_addFilter, world.Has<SceneTransientComponent>(entity)))
			{
				// Empty entt component: World::Emplace can't return a reference
				// to it, so go through the registry directly.
				world.GetRegistry().emplace<SceneTransientComponent>(World::ToEntt(entity));
			}
			ImGui::EndPopup();
		}
		ImGui::Separator();

		// ── Component sections ─────────────────────────────────────────────────
		DrawTransform(context, world, entity);
		DrawSkinnedMesh(world, entity);
		DrawMaterial(context, world, entity);
		DrawEffectParams(context, world, entity);
		DrawLights(world, entity);
		DrawScript(world, entity);
		DrawBehaviors(world, entity);
		DrawPhysics(world, entity);
		DrawMeshPipeline(world, entity);
		DrawHierarchy(world, entity, selection);
		DrawTags(world, entity, m_addTagBuf, sizeof(m_addTagBuf));
		DrawSceneTransient(world, entity);

		ImGui::End();
	}
} // namespace aether::app
