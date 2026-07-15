#include "debug/InspectorPanel.hpp"
#include "debug/EditorChrome.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

#include "Color.hpp"
#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/ReflectedComponentDrawer.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "material/EffectManager.hpp"
#include "utils/Logger.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialAsset.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "physics/PhysicsComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/Entity.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor
{
	namespace
	{
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
				if (!lower.contains(needle))
				{
					return false;
				}
			}
			return ImGui::MenuItem(label);
		}

		void ReleaseMaterialAssetTextures(AssetManager& assets, const MaterialAsset& material)
		{
			auto& textures = assets.GetTextureRegistry();
			for (const TextureHandle h: {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
			{
				if (h.IsValid())
				{
					textures.Release(h);
				}
			}
		}

		bool AssignMaterialPreset(app::LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			if (assets == nullptr)
			{
				return false;
			}
			auto loaded = assets->LoadMaterialPreset(path);
			if (!loaded)
			{
				return false;
			}
			const MaterialAsset material = loaded.value();
			MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), material);
			ReleaseMaterialAssetTextures(*assets, material);
			if (auto* db = context.TryGet<AssetDatabase>())
			{
				db->Register(MakeMaterialPresetSource(std::string(path)));
			}
			return true;
		}

		bool AssignTextureToEntity(app::LayerContext& context, World& world, Entity entity, std::string_view path)
		{
			auto* assets = context.TryGet<AssetManager>();
			if (assets == nullptr)
			{
				return false;
			}
			const TextureHandle texture = assets->GetTextureRegistry().Acquire(path);
			MaterialSystem::SetAlbedoTexture(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), texture);
			if (texture.IsValid())
			{
				assets->GetTextureRegistry().Release(texture);
			}
			return true;
		}

		bool AssignModelAsset(app::LayerContext& context, World& world, Entity entity, const std::string& path)
		{
			auto* assets = context.TryGet<AssetManager>();
			auto* sceneCtx = context.TryGet<app::scripting::SceneContext>();
			if (assets == nullptr || sceneCtx == nullptr)
			{
				return false;
			}
			if (const auto* project = context.TryGet<app::EditorProjectContext>())
			{
				std::string bakeError;
				if (!editor::EnsureModelBaked(path, *project, bakeError))
				{
					AE_WARN(LogCategory::App, "Model import failed for '{}': {}", path, bakeError);
					return false;
				}
			}
			return app::scene::AssignModelToEntity(world, *assets, *sceneCtx, entity, path);
		}

		Entity InstantiatePrefabAsset(app::LayerContext& context, World& world, const std::string& name, Entity parent = {})
		{
			if (const auto prefab = app::scene::ReadPrefabFile(name))
			{
				const Entity root = app::scene::InstantiatePrefab(*prefab, world, app::scene::MakeApplySceneDeps(context.services), glm::mat4(1.0f));
				if (root.IsValid() && parent.IsValid())
				{
					ecs::SetParent(world, root, parent);
				}
				return root;
			}
			return {};
		}

		bool IsEntityAlive(const World& world, Entity entity)
		{
			return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
		}

		std::string EntityTargetLabel(const World& world, Entity entity)
		{
			if (const auto* name = world.TryGet<NameComponent>(entity); name != nullptr && !name->name.empty())
			{
				return name->name;
			}
			return "Entity #" + std::to_string(entity.id);
		}

		bool ApplyFilePayloadToEntity(app::LayerContext& context, World& world, SceneSelection& selection, Entity entity, const dragdrop::FilePayload& payload)
		{
			switch (payload.kind)
			{
				case dragdrop::FileKind::Model:
					if (AssignModelAsset(context, world, entity, payload.path))
					{
						selection.Select(entity);
						return true;
					}
					return false;
				case dragdrop::FileKind::Prefab:
				{
					const Entity root = InstantiatePrefabAsset(context, world, payload.path, entity);
					if (root.IsValid())
					{
						selection.Select(root);
						return true;
					}
					return false;
				}
				case dragdrop::FileKind::Material:
					return AssignMaterialPreset(context, world, entity, payload.path);
				case dragdrop::FileKind::Texture:
					return AssignTextureToEntity(context, world, entity, payload.path);
				case dragdrop::FileKind::Unknown:
				case dragdrop::FileKind::Script:
				case dragdrop::FileKind::Scene:
				default:
					return false;
			}
		}

		const char* AssetKindLabel(SceneSelection::AssetKind kind)
		{
			switch (kind)
			{
				case SceneSelection::AssetKind::Model:
					return "Model";
				case SceneSelection::AssetKind::Material:
					return "Material";
				case SceneSelection::AssetKind::Texture:
					return "Texture";
				case SceneSelection::AssetKind::Script:
					return "Script";
				case SceneSelection::AssetKind::Prefab:
					return "Prefab";
				case SceneSelection::AssetKind::Scene:
					return "Scene";
				case SceneSelection::AssetKind::File:
					return "File";
				case SceneSelection::AssetKind::None:
				default:
					return "Asset";
			}
		}

		const char* AssetKindIcon(SceneSelection::AssetKind kind)
		{
			switch (kind)
			{
				case SceneSelection::AssetKind::Model:
					return ICON_FA_PERSON_RUNNING;
				case SceneSelection::AssetKind::Material:
					return ICON_FA_PALETTE;
				case SceneSelection::AssetKind::Texture:
					return ICON_FA_IMAGE;
				case SceneSelection::AssetKind::Script:
					return ICON_FA_CODE;
				case SceneSelection::AssetKind::Prefab:
					return ICON_FA_BOX_OPEN;
				case SceneSelection::AssetKind::Scene:
					return ICON_FA_FOLDER_OPEN;
				case SceneSelection::AssetKind::File:
				case SceneSelection::AssetKind::None:
				default:
					return ICON_FA_IMAGE;
			}
		}

		[[maybe_unused]] void DrawAssetInspector(app::LayerContext& context, World& world, SceneSelection& selection)
		{
			const SceneSelection::Asset& asset = selection.SelectedAsset();
			const Entity target = selection.LastEntityPrimary();
			const bool hasTarget = IsEntityAlive(world, target);
			ImGui::Text("%s  %s", AssetKindIcon(asset.kind), asset.displayName.c_str());
			ImGui::TextDisabled("%s", AssetKindLabel(asset.kind));
			ImGui::Separator();
			ImGui::TextWrapped("%s", asset.path.c_str());
			ImGui::Separator();
			if (hasTarget)
			{
				const std::string targetLabel = EntityTargetLabel(world, target);
				ImGui::TextDisabled("Target  %s", targetLabel.c_str());
				ImGui::Separator();
			}

			if (asset.kind == SceneSelection::AssetKind::Model)
			{
				ImGui::BeginDisabled(!hasTarget);
				if (ImGui::Button(ICON_FA_PERSON_RUNNING "  Apply to Target"))
				{
					if (AssignModelAsset(context, world, target, asset.path))
					{
						selection.Select(target);
					}
				}
				ImGui::EndDisabled();
				if (!hasTarget)
				{
					ImGui::TextDisabled("Select an entity to assign this model.");
				}
			}
			else if (asset.kind == SceneSelection::AssetKind::Prefab)
			{
				if (ImGui::Button(ICON_FA_PLUS "  Instantiate"))
				{
					if (const Entity root = InstantiatePrefabAsset(context, world, asset.path); root.IsValid())
					{
						selection.Select(root);
					}
				}
				if (hasTarget)
				{
					ImGui::SameLine();
					if (ImGui::Button(ICON_FA_SITEMAP "  Instantiate Under Target"))
					{
						if (const Entity root = InstantiatePrefabAsset(context, world, asset.path, target); root.IsValid())
						{
							selection.Select(root);
						}
					}
				}
			}
			else if (asset.kind == SceneSelection::AssetKind::Material)
			{
				ImGui::BeginDisabled(!hasTarget);
				if (ImGui::Button(ICON_FA_PALETTE "  Apply Material"))
				{
					if (AssignMaterialPreset(context, world, target, asset.path))
					{
						selection.Select(target);
					}
				}
				ImGui::EndDisabled();
			}
			else if (asset.kind == SceneSelection::AssetKind::Texture)
			{
				ImGui::BeginDisabled(!hasTarget);
				if (ImGui::Button(ICON_FA_IMAGE "  Apply Albedo"))
				{
					if (AssignTextureToEntity(context, world, target, asset.path))
					{
						selection.Select(target);
					}
				}
				ImGui::EndDisabled();
			}
			else if (asset.kind == SceneSelection::AssetKind::Script)
			{
				const std::string typeName = std::filesystem::path(asset.path).stem().generic_string();
				ImGui::TextDisabled("Type  %s", typeName.c_str());
				ImGui::BeginDisabled(!hasTarget);
				if (ImGui::Button(ICON_FA_CODE "  Add Script"))
				{
					AddScriptToEntity(world, target, typeName);
					selection.Select(target);
				}
				ImGui::EndDisabled();
			}
		}
	} // namespace

	bool InspectorPanel::IsAlive(const World& world, Entity entity)
	{
		return entity.IsValid() && world.GetRegistry().valid(World::ToEntt(entity));
	}

	void InspectorPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();

		ImGui::Begin("Inspector", VisiblePtr());
		World& world = context.Get<World>();
		auto& selection = context.Get<SceneSelection>();
		Entity entity = selection.Primary();
		if (!IsAlive(world, entity) && selection.HasAsset() && IsAlive(world, selection.LastEntityPrimary()))
		{
			entity = selection.LastEntityPrimary();
		}

		if (!IsAlive(world, entity))
		{
			ImGui::Dummy(ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.4f));
			const char* icon = ICON_FA_CIRCLE_INFO;
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(icon).x) * 0.5f);
			ImGui::TextDisabled("%s", icon);
			const char* msg = "Select an entity to inspect it";
			ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) * 0.5f);
			ImGui::TextDisabled("%s", msg);
			ImGui::End();
			return;
		}

		char idText[64]{};
		if (selection.All().size() > 1)
		{
			std::snprintf(idText, sizeof(idText), "#%u \xC2\xB7 %zu SELECTED", entity.id, selection.All().size());
		}
		else
		{
			std::snprintf(idText, sizeof(idText), "#%u", entity.id);
		}
		const KindBadge badge = EntityKindBadge(world, entity);
		{
			bool active = !world.Has<DisabledComponent>(entity);
			ImGui::AlignTextToFramePadding();
			if (ImGui::Checkbox("##active", &active))
			{
				const bool disable = !active;
				auto applyActive = [&](Entity target)
				{
					if (disable)
					{
						world.EmplaceOrReplace<DisabledComponent>(target);
					}
					else if (world.Has<DisabledComponent>(target))
					{
						world.Remove<DisabledComponent>(target);
					}
				};
				if (selection.All().size() > 1 && selection.Contains(entity))
				{
					for (const Entity target: selection.All())
					{
						if (IsAlive(world, target))
						{
							applyActive(target);
						}
					}
				}
				else
				{
					applyActive(entity);
				}
			}
			ImGui::SetItemTooltip("Active - uncheck to disable this entity and its children");
			ImGui::SameLine();

			ImGui::PushFont(nullptr, 18.0f);
			const float idW = ImGui::CalcTextSize(idText).x;
			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(badge.color, "%s", badge.icon);
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, iw::WithAlpha(colors::Orange, 0.08f));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, iw::WithAlpha(colors::Orange, 0.12f));
			if (auto* nc = world.TryGet<NameComponent>(entity))
			{
				char buf[128];
				std::snprintf(buf, sizeof(buf), "%s", nc->name.c_str());
				ImGui::SetNextItemWidth(-(idW + ImGui::GetStyle().ItemSpacing.x));
				if (ImGui::InputText("##name", buf, sizeof(buf)))
				{
					nc->name = buf;
				}
			}
			else if (ImGui::SmallButton(ICON_FA_PEN "  Name this entity"))
			{
				world.Emplace<NameComponent>(entity, NameComponent{.name = "Entity"});
			}
			ImGui::SameLine(ImGui::GetContentRegionMax().x - idW);
			ImGui::TextDisabled("%s", idText);
			ImGui::PopStyleColor(3);
			ImGui::PopFont();
		}

		const float trashW = ImGui::GetFrameHeight() + 8.0f;
		const float toolWidth = ImGui::GetContentRegionAvail().x - trashW - ImGui::GetStyle().ItemSpacing.x;
		if (iw::AccentButton(ICON_FA_PLUS "  Add Component", ImVec2(toolWidth, 0.0f)))
		{
			m_addFilter[0] = '\0';
			m_addFocusPending = true;
			ImGui::OpenPopup("AddComponent");
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, iw::WithAlpha(colors::Red, 0.8f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, iw::WithAlpha(colors::Red, 0.28f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, iw::WithAlpha(colors::Red, 0.45f));
		const bool deleteClicked = ImGui::Button("##deleteEntity", ImVec2(trashW, 0.0f));
		ImGui::PopStyleColor(4);
		chrome::CenterIconOnLastItem(ICON_FA_TRASH, iw::WithAlpha(colors::Red, 0.8f));
		ImGui::SetItemTooltip("Delete entity (and children)");
		if (deleteClicked)
		{
			ecs::DestroyHierarchy(world, entity);
			selection.Clear();
			ImGui::End();
			return;
		}

		chrome::AccentHairline(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, 0.25f);
		ImGui::Dummy(ImVec2(0.0f, 5.0f));

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
			auto* sceneCtx = context.TryGet<app::scripting::SceneContext>();

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
				world.EmplaceOrReplace<MeshRendererComponent>(entity);
			};
			addPrimitive(PrimitiveMesh::Cube, ICON_FA_CUBE "  Mesh Renderer - Cube", "cube");
			addPrimitive(PrimitiveMesh::Sphere, ICON_FA_CIRCLE "  Mesh Renderer - Sphere", "sphere");
			addPrimitive(PrimitiveMesh::Plane, ICON_FA_IMAGE "  Mesh Renderer - Plane", "plane");
			addPrimitive(PrimitiveMesh::Quad, ICON_FA_IMAGE "  Mesh Renderer - Quad", "quad");
			addPrimitive(PrimitiveMesh::Triangle, ICON_FA_PLAY "  Mesh Renderer - Triangle", "triangle");

			if (PaletteEntry(ICON_FA_IMAGE "  Sprite Renderer (2D)", m_addFilter, world.Has<SpriteRendererComponent>(entity)))
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.EmplaceOrReplace<SpriteRendererComponent>(entity);
			}
			if (PaletteEntry(ICON_FA_FILM "  Sprite Animator (2D)", m_addFilter, world.Has<SpriteAnimatorComponent>(entity)))
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				if (!world.Has<SpriteRendererComponent>(entity))
				{
					world.Emplace<SpriteRendererComponent>(entity);
				}
				world.EmplaceOrReplace<SpriteAnimatorComponent>(entity);
			}

			if (m_addFilter[0] == '\0')
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextWrapped(ICON_FA_CUBE "  glTF Model: drag a .gltf/.glb from the File Explorer onto the entity or its Mesh Renderer slot");
				ImGui::PopStyleColor();
			}
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
			if (PaletteEntry(ICON_FA_VIDEO "  Camera", m_addFilter, world.Has<CameraComponent>(entity)))
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.Emplace<CameraComponent>(entity);
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

			{
				ImGui::SeparatorText("Scripts");
				if (PaletteEntry(ICON_FA_CODE "  Script", m_addFilter, false))
				{
					AddScriptToEntity(world, entity);
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
				const float radius = std::max(std::sqrt(curPos.x * curPos.x + curPos.z * curPos.z), 3.0f);
				const float angleDeg = glm::degrees(std::atan2(curPos.z, curPos.x));
				world.Emplace<OrbitComponent>(entity, OrbitComponent{.center = {0.0f, 0.0f, 0.0f}, .radius = radius, .angularSpeedDeg = 30.0f, .angleDeg = angleDeg, .yawOffsetDeg = 0.0f, .height = curPos.y});
			}
			if (PaletteEntry(ICON_FA_HEART_PULSE "  Material Pulse", m_addFilter, world.Has<MaterialPulseComponent>(entity)))
			{
				world.Emplace<MaterialPulseComponent>(entity, MaterialPulseComponent{.emissiveA = {0.0f, 0.0f, 0.05f}, .emissiveB = {0.9f, 0.2f, 0.05f}, .frequency = 2.0f});
			}
			if (PaletteEntry(ICON_FA_EXPAND "  Scale Pulse", m_addFilter, world.Has<ScalePulseComponent>(entity)))
			{
				world.Emplace<ScalePulseComponent>(entity, ScalePulseComponent{.amplitude = 0.2f, .frequency = 2.0f});
			}
			if (PaletteEntry(ICON_FA_EYE "  Look At", m_addFilter, world.Has<LookAtComponent>(entity)))
			{
				world.Emplace<LookAtComponent>(entity, LookAtComponent{.target = {0.0f, 0.0f, 0.0f}});
			}

			ImGui::SeparatorText("Physics");
			const bool hasCollider = world.Has<ColliderComponent>(entity);
			const bool hasRigidBody = world.Has<RigidBodyComponent>(entity);

			if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Rigid Body", m_addFilter, hasRigidBody))
			{
				world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
			}
			const auto addCollider = [&](PhysicsShapeType shape, const char* label)
			{
				if (!PaletteEntry(label, m_addFilter, hasCollider))
				{
					return;
				}
				ColliderComponent c{};
				c.shape = shape;
				c.halfExtents = curScale * 0.5f;
				c.radius = shape == PhysicsShapeType::Sphere ? std::max({curScale.x, curScale.y, curScale.z}) * 0.5f : curScale.x * 0.5f;
				c.halfHeight = curScale.y * 0.5f;
				world.Emplace<ColliderComponent>(entity, c);
			};
			addCollider(PhysicsShapeType::Box, ICON_FA_WEIGHT_HANGING "  Box Collider");
			addCollider(PhysicsShapeType::Sphere, ICON_FA_WEIGHT_HANGING "  Sphere Collider");
			addCollider(PhysicsShapeType::Capsule, ICON_FA_WEIGHT_HANGING "  Capsule Collider");
			addCollider(PhysicsShapeType::Cylinder, ICON_FA_WEIGHT_HANGING "  Cylinder Collider");
			if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Trigger Volume (box sensor)", m_addFilter, hasCollider))
			{
				ColliderComponent c{};
				c.shape = PhysicsShapeType::Box;
				c.halfExtents = curScale * 0.5f;
				c.isSensor = true;
				c.layer = PhysicsLayer::Sensor;
				world.Emplace<ColliderComponent>(entity, c);
			}
			if (PaletteEntry(ICON_FA_LINK "  Joint", m_addFilter, world.Has<JointComponent>(entity)))
			{
				world.Emplace<JointComponent>(entity, JointComponent{.anchor = curPos});
			}
			if (PaletteEntry(ICON_FA_BOLT "  Collision Events", m_addFilter, world.Has<CollisionEventsComponent>(entity)))
			{
				world.GetRegistry().emplace<CollisionEventsComponent>(World::ToEntt(entity));
			}

			ImGui::SeparatorText("Editor");
			if (PaletteEntry(ICON_FA_GHOST "  Scene Transient", m_addFilter, world.Has<SceneTransientComponent>(entity)))
			{
				world.GetRegistry().emplace<SceneTransientComponent>(World::ToEntt(entity));
			}
			ImGui::EndPopup();
		}
		ImGui::Separator();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 6.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
		DrawTransform(context, world, entity);
		DrawSkinnedMesh(world, entity);
		DrawMaterial(context, world, entity);
		DrawEffectParams(context, world, entity);
		DrawUiCanvas(world, entity);
		DrawUiRect(world, entity);
		DrawUiImage(world, entity);
		DrawUiText(world, entity);
		DrawCamera(world, entity);
		DrawScript(context, world, entity);
		DrawReflectedComponents(world, entity, {"Transform", "Skinned Mesh", "Material", "Camera", "Rigid Body", "Collider", "Joint", "Name", "Sprite Renderer", "Sprite Animator"});
		DrawPhysics(context, world, entity);
		DrawJoint(context, world, entity);
		DrawCollisionEvents(world, entity);
		DrawMeshRenderer(context, world, entity);
		DrawSpriteRenderer(context, world, entity);
		DrawSpriteAnimator(context, world, entity);
		DrawHierarchy(world, entity, selection);
		DrawTags(world, entity, m_addTagBuf, sizeof(m_addTagBuf));
		DrawSceneTransient(world, entity);
		ImGui::PopStyleVar(2);

		if (const ImGuiPayload* activePayload = ImGui::GetDragDropPayload(); activePayload != nullptr && (activePayload->IsDataType(dragdrop::kScriptPayload) || activePayload->IsDataType(dragdrop::kFilePayload)))
		{
			const ImVec2 windowPos = ImGui::GetWindowPos();
			const ImVec2 windowSize = ImGui::GetWindowSize();
			const ImRect dropRect(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y));
			ImGui::GetWindowDrawList()->AddRect(ImVec2(dropRect.Min.x + 3.0f, dropRect.Min.y + 3.0f), ImVec2(dropRect.Max.x - 3.0f, dropRect.Max.y - 3.0f), chrome::U32(chrome::WithAlpha(chrome::kDropTarget, 0.72f)), 4.0f, 0, 2.0f);
			if (ImGui::BeginDragDropTargetCustom(dropRect, ImGui::GetID("##inspectorScriptDropTarget")))
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kScriptPayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
				{
					if (payload->DataSize == sizeof(dragdrop::ScriptPayload))
					{
						const auto* script = static_cast<const dragdrop::ScriptPayload*>(payload->Data);
						AddScriptToEntity(world, entity, script->typeName);
					}
				}
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload, ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
				{
					if (payload->DataSize == sizeof(dragdrop::FilePayload))
					{
						const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
						ApplyFilePayloadToEntity(context, world, selection, entity, *file);
					}
				}
				ImGui::EndDragDropTarget();
			}
		}

		ImGui::End();
	}
} // namespace aether::editor
