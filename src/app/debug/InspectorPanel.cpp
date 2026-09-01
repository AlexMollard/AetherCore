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
#include "debug/MaterialAssetEditor.hpp"
#include "io/FileSystem.hpp"
#include "assets/AssetManager.hpp"
#include "assets/AssetTypes.hpp"
#include "debug/ComponentDrawers.hpp"
#include "debug/ReflectedComponentDrawer.hpp"
#include "debug/UndoStack.hpp"
#include "debug/EditorDragDrop.hpp"
#include "debug/Icons.hpp"
#include "debug/InspectorWidgets.hpp"
#include "debug/SceneSelection.hpp"
#include "editor/ComponentCatalog.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "material/EffectManager.hpp"
#include "utils/Logger.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialAsset.hpp"
#include "ui/UiComponents.hpp"
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
			// Linked, not copied: MaterialLinkComponent is what lets a later edit to the asset
			// reach this entity. A fresh assignment clears any overrides the previous material
			// had accumulated, because they described fields of a different material.
			world.EmplaceOrReplace<MaterialLinkComponent>(entity, MaterialLinkComponent{.assetPath = std::string(path)});
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
				// Linked instance (not a copy): edits to the prefab propagate.
				const Entity root = app::scene::InstantiatePrefabInstance(name, *prefab, world, app::scene::MakeApplySceneDeps(context.services), glm::mat4(1.0f));
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

		// Drop a .slang onto a UI Effect entity to set its shader (by stem, e.g. "ui_ink").
		bool AssignShaderToEntity(World& world, Entity entity, const std::string& path)
		{
			auto* fx = world.TryGet<ui::UIEffect>(entity);
			if (fx == nullptr)
			{
				return false;
			}
			fx->shader = std::filesystem::path(path).stem().string();
			return true;
		}

		bool ApplyFilePayloadToEntity(app::LayerContext& context, World& world, SceneSelection& selection, Entity entity, const dragdrop::FilePayload& payload)
		{
			switch (payload.kind)
			{
				case dragdrop::FileKind::Shader:
					return AssignShaderToEntity(world, entity, payload.path);
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

		// Selecting a file does NOT take the Inspector over. The Material and Texture windows
		// still follow the asset selection, so the asset is not unreachable - but clicking
		// around the content browser no longer throws away the entity you were working on,
		// which is the thing you are usually still editing.
		const Entity entity = selection.Primary();

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
				// Recorded, and as one entry across the selection: without it the toggle left
				// no history and the next Ctrl+Z reached past it - disabling an entity and
				// undoing could delete the entity instead of re-enabling it.
				auto* activeUndo = context.services.TryGet<UndoStack>();
				UndoStack::ScopedGroup activeGroup(activeUndo, disable ? "Disable" : "Enable");
				auto applyActive = [&](Entity target)
				{
					const bool wasDisabled = world.Has<DisabledComponent>(target);
					if (wasDisabled == disable)
					{
						return; // already in the requested state - not an edit
					}
					if (activeUndo != nullptr)
					{
						activeUndo->Record(std::make_unique<SetEnabledCommand>(target.id, wasDisabled));
					}
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
					// Record through the same coalescing buffer the reflected drawer uses, so a
					// whole typing session collapses into one step. This header field is drawn by
					// hand rather than by the reflected pass, so it was outside that hook entirely
					// and renaming from the Inspector recorded nothing at all - undo silently
					// skipped past it to whatever came before.
					const std::string before = nc->name;
					nc->name = buf;
					if (auto* undo = context.TryGet<UndoStack>(); undo != nullptr && before != nc->name)
					{
						undo->RecordFieldEdit(entity.id, "Name", "name", nlohmann::json(before), nlohmann::json(nc->name), /*isReflected=*/true);
					}
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

		// A transient entity is deliberately left out of the saved scene. That is the whole
		// point of the marker, but from the Inspector it is invisible: the entity looks exactly
		// like any other right up until it is not in the file. Say so where the decision is
		// made, rather than letting a save quietly drop it.
		if (world.Has<SceneTransientComponent>(entity))
		{
			ImGui::PushStyleColor(ImGuiCol_Text, iw::WithAlpha(colors::Orange, 0.95f));
			ImGui::TextWrapped("%s  Not saved with the scene - this entity is marked Scene Transient.", ICON_FA_GHOST);
			ImGui::PopStyleColor();
			ImGui::Spacing();
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

			// Unity-style palette: one entry per component, everything available
			// in every scene. Variants (mesh shape, collider shape, effect name)
			// are switched in the component's own drawer after adding.
			ImGui::SeparatorText("Rendering");
			if (PaletteEntry(ICON_FA_CUBE "  Mesh Renderer", m_addFilter, world.Has<MeshComponent>(entity)) && primitives != nullptr && assets != nullptr)
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.Emplace<MeshComponent>(entity, MeshComponent{.mesh = &primitives->Get(PrimitiveMesh::Cube)});
				world.EmplaceOrReplace<MeshSourceComponent>(entity, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = "cube", .primitiveIndex = 0});
				if (!world.Has<MaterialComponent>(entity))
				{
					MaterialAsset asset{};
					asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
					asset.roughnessFactor = 0.6f;
					asset.doubleSided = true;
					MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
				}
				world.EmplaceOrReplace<MeshRendererComponent>(entity);
			}
			if (PaletteEntry(ICON_FA_IMAGE "  Sprite Renderer", m_addFilter, world.Has<SpriteRendererComponent>(entity)))
			{
				if (!world.Has<TransformComponent>(entity))
				{
					world.Emplace<TransformComponent>(entity);
				}
				world.EmplaceOrReplace<SpriteRendererComponent>(entity);
			}
			if (PaletteEntry(ICON_FA_FILM "  Sprite Animator", m_addFilter, world.Has<SpriteAnimatorComponent>(entity)))
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
			if (PaletteEntry(ICON_FA_PALETTE "  Material", m_addFilter, world.Has<MaterialComponent>(entity)) && assets != nullptr)
			{
				MaterialAsset asset{};
				asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
				asset.roughnessFactor = 0.6f;
				MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), asset);
			}
			if (sceneCtx != nullptr && sceneCtx->effects != nullptr && assets != nullptr)
			{
				std::vector<std::string> effectNames;
				sceneCtx->effects->ForEachEffect([&](const std::string& n, const auto&) { effectNames.push_back(n); });
				std::sort(effectNames.begin(), effectNames.end());
				if (!effectNames.empty() && PaletteEntry(ICON_FA_BOLT "  Effect", m_addFilter, world.Has<EffectRefComponent>(entity)))
				{
					effects::ApplyEntityEffect(world, entity, effectNames.front(), *sceneCtx->effects, assets->GetPipelineCache(), assets->GetEffectParamBuffer());
				}
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
			if (m_addFilter[0] == '\0')
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextWrapped(ICON_FA_CUBE "  glTF Model: drag a .gltf/.glb from the File Explorer onto the entity or its Mesh Renderer slot");
				ImGui::PopStyleColor();
			}

			{
				ImGui::SeparatorText("Scripts");
				if (PaletteEntry(ICON_FA_CODE "  Script", m_addFilter, false))
				{
					AddScriptToEntity(world, entity);
				}
			}

			// Domain conflicts (an entity never simulates 2D and 3D physics at
			// once) hide the opposing physics section through the shared rule.
			const auto physicsVisible = [&](const char* entryName)
			{
				const editor::ComponentCatalogEntry* entry = editor::FindComponent(entryName);
				return entry != nullptr && editor::ComponentVisibleInMenu(world, entity, *entry);
			};

			if (physicsVisible("Rigid Body"))
			{
				ImGui::SeparatorText("Physics");
				if (PaletteEntry(ICON_FA_WEIGHT_HANGING "  Rigid Body", m_addFilter, world.Has<RigidBodyComponent>(entity)))
				{
					world.Emplace<RigidBodyComponent>(entity, RigidBodyComponent{.motionType = PhysicsMotionType::Dynamic});
				}
				if (PaletteEntry(ICON_FA_DRAW_POLYGON "  Collider", m_addFilter, world.Has<ColliderComponent>(entity)))
				{
					ColliderComponent c{};
					c.shape = PhysicsShapeType::Box;
					c.halfExtents = curScale * 0.5f;
					c.radius = curScale.x * 0.5f;
					c.halfHeight = curScale.y * 0.5f;
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
			}

			if (physicsVisible("Rigid Body 2D"))
			{
				ImGui::SeparatorText("Physics 2D");
				const auto add2D = [&](const char* entryName, const char* label)
				{
					const editor::ComponentCatalogEntry* entry = editor::FindComponent(entryName);
					if (entry == nullptr || entry->has == nullptr || entry->add == nullptr)
					{
						return;
					}
					if (PaletteEntry(label, m_addFilter, entry->has(world, entity)))
					{
						if (!world.Has<TransformComponent>(entity))
						{
							world.Emplace<TransformComponent>(entity);
						}
						entry->add(world, entity, context.services);
					}
				};
				add2D("Rigid Body 2D", ICON_FA_WEIGHT_HANGING "  Rigid Body 2D");
				add2D("Collider 2D", ICON_FA_BOX_OPEN "  Collider 2D");
				add2D("Joint 2D", ICON_FA_LINK "  Joint 2D");
			}

			// Catalog components without a hand-authored entry above (reflected
			// registrations like Day Night or future 2D physics additions) list
			// here automatically, honoring the same feature/conflict visibility.
			{
				static constexpr std::string_view kHandAuthored[] = {"Bob",
				        "Box Collider",
				        "Camera",
				        "Capsule Collider",
				        "Collision Events",
				        "Cylinder Collider",
				        "Hierarchy",
				        "Joint",
				        "Look At",
				        "Material",
				        "Material Pulse",
				        "Name",
				        "Orbit",
				        "Point Light",
				        "Rigid Body",
				        "Scale Pulse",
				        "Scene Transient",
				        "Sphere Collider",
				        "Spin",
				        "Spot Light",
				        "Sprite Animator",
				        "Sprite Renderer",
				        "Trigger Volume",
				        "UI Text",
				        "Transform",
				        "Rigid Body 2D",
				        "Collider 2D",
				        "Joint 2D"};
				bool headerShown = false;
				for (const editor::ComponentCatalogEntry& entry: editor::ComponentCatalog())
				{
					if (std::find(std::begin(kHandAuthored), std::end(kHandAuthored), entry.name) != std::end(kHandAuthored))
					{
						continue;
					}
					if (entry.has == nullptr || entry.add == nullptr || !editor::ComponentVisibleInMenu(world, entity, entry))
					{
						continue;
					}
					if (!headerShown)
					{
						ImGui::SeparatorText("Components");
						headerShown = true;
					}
					if (PaletteEntry((entry.icon + "  " + entry.name).c_str(), m_addFilter, entry.has(world, entity)))
					{
						auto* addUndo = context.TryGet<UndoStack>();
						// The implicit Transform is recorded separately so undoing the
						// add does not strand a component the user never asked for.
						if (!world.Has<TransformComponent>(entity))
						{
							world.Emplace<TransformComponent>(entity);
							if (addUndo != nullptr)
							{
								addUndo->Record(std::make_unique<AddComponentCommand>(entity.id, "Transform"));
							}
						}
						entry.add(world, entity, context.services);
						if (addUndo != nullptr)
						{
							addUndo->Record(std::make_unique<AddComponentCommand>(entity.id, entry.name));
						}
					}
				}
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

		// The bespoke drawers below mutate components directly, so their edits are
		// attributed by diffing the entity's reflected fields around the whole block
		// instead of hooking each widget. Only worth doing on frames where a widget is
		// actually being driven; on any frame this misses, the edit still falls back to
		// the scene-diff safety net rather than being lost.
		auto* fieldUndo = context.TryGet<UndoStack>();
		const ImGuiIO& inspectorIo = ImGui::GetIO();
		const bool watchFieldEdits = fieldUndo != nullptr && (ImGui::IsAnyItemActive() || inspectorIo.MouseDown[0] || inspectorIo.MouseReleased[0]);
		// Every selected entity, not just the inspected one: the transform drawer
		// applies its delta across the whole selection, and recording only the primary
		// would drop the baseline and lose the rest.
		std::vector<std::pair<Entity, std::vector<ReflectedComponentFields>>> fieldsBefore;
		std::vector<ScriptEntry> scriptsBefore;
		std::vector<std::uint32_t> tagsBefore;
		if (watchFieldEdits)
		{
			scriptsBefore = CaptureScripts(world, entity);
			tagsBefore = CaptureTags(world, entity);
			fieldsBefore.emplace_back(entity, CaptureReflectedFields(world, entity, context.services));
			for (const Entity other: selection.All())
			{
				if (other != entity && world.GetRegistry().valid(World::ToEntt(other)))
				{
					fieldsBefore.emplace_back(other, CaptureReflectedFields(world, other, context.services));
				}
			}
		}

		DrawTransform(context, world, entity);
		DrawSkinnedMesh(world, entity);
		DrawMaterial(context, world, entity);
		DrawEffectParams(context, world, entity);
		DrawUiCanvas(world, entity);
		DrawUiRect(world, entity);
		DrawUiImage(context, world, entity);
		DrawUiText(world, entity);
		DrawCamera(context, world, entity);
		DrawScript(context, world, entity);
		// UI Canvas/Rect/Image/Text have richer bespoke drawers above; exclude them here so the
		// reflected pass does not draw them a second time. UI Slider/Toggle/Button/Progress Bar and
		// UI Selectable have no bespoke drawer, so they are intentionally drawn by the reflected pass.
		DrawReflectedComponents(world, entity, context.services, {"Transform", "Skinned Mesh", "Material", "Camera", "Rigid Body", "Collider", "Joint", "Name", "Sprite Renderer", "Sprite Animator", "UI Canvas", "UI Rect", "UI Image", "UI Text"});
		DrawCollider2DTools(context, world, entity);
		DrawPhysics(context, world, entity);
		DrawJoint(context, world, entity);
		DrawCollisionEvents(world, entity);
		DrawMeshRenderer(context, world, entity);
		DrawSpriteRenderer(context, world, entity);
		DrawSpriteAnimator(context, world, entity);
		DrawHierarchy(world, entity, selection);
		DrawTags(world, entity, m_addTagBuf, sizeof(m_addTagBuf));
		DrawSceneTransient(world, entity);
		for (const auto& [snapshotEntity, snapshotFields]: fieldsBefore)
		{
			if (world.GetRegistry().valid(World::ToEntt(snapshotEntity)))
			{
				RecordReflectedFieldEdits(*fieldUndo, world, snapshotEntity, context.services, snapshotFields);
			}
		}
		if (watchFieldEdits && world.GetRegistry().valid(World::ToEntt(entity)))
		{
			RecordScriptEdits(*fieldUndo, world, entity, scriptsBefore);
			RecordTagEdits(*fieldUndo, world, entity, tagsBefore);
		}
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
