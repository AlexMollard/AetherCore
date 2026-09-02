#include "debug/ComponentDrawers.hpp"
#include "debug/EditorCommand.hpp"
#include "debug/UndoStack.hpp"

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
#include "debug/ReflectedComponentDrawer.hpp"
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
	void DrawSkinnedMesh(World& world, Entity entity)
	{
		auto* smc = world.TryGet<SkinnedMeshComponent>(entity);
		if (!smc || !SectionHeader(ICON_FA_FILM "  Skinned Mesh"))
		{
			return;
		}

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
				part->animTime = 0.0f;
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
	namespace
	{
		AssetId AssetPickerButton(const char* popupId, AssetDatabase* db, AssetType type, AssetId current, const char* emptyLabel, const char* explicitLabel = nullptr);
	}

	void DrawMaterial(app::LayerContext& context, World& world, Entity entity)
	{
		auto* mc = world.TryGet<MaterialComponent>(entity);
		if (!mc || !SectionHeader(ICON_FA_PALETTE "  Material", ImGuiTreeNodeFlags_DefaultOpen, MenuFor(context, world, entity, "Material")))
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
		auto* assetDb = context.TryGet<AssetDatabase>();

		bool changed = false;

		iw::PropLabel("Preset");
		if (const AssetId pk = AssetPickerButton("##matPresetPicker", assetDb, AssetType::Material, AssetId{}, "Load a preset..."); pk.IsValid() && assetDb != nullptr)
		{
			AssetSource src;
			if (assetDb->Describe(pk, src))
			{
				if (auto loaded = assets->LoadMaterialPreset(src.path))
				{
					asset = loaded.value();
					for (const TextureHandle h: {asset.albedoTex, asset.normalTex, asset.metallicRoughnessTex, asset.occlusionTex, asset.emissiveTex})
					{
						if (h.IsValid())
						{
							transientTextureRefs.push_back(h);
						}
					}
					changed = true;
				}
			}
		}
		iw::ItemTooltip("Replace this material with a catalogued preset");

		auto* link = world.TryGet<MaterialLinkComponent>(entity);
		if (link != nullptr && !link->assetPath.empty())
		{
			// A linked material's values come from the asset. Editing one here does not
			// change the asset - it overrides that one field on this object, and the field is
			// listed so the next asset edit knows to leave it alone.
			iw::PropLabel("Linked");
			ImGui::TextUnformatted(std::filesystem::path(link->assetPath).filename().generic_string().c_str());
			ImGui::SetItemTooltip("%s", link->assetPath.c_str());
			if (!link->overrides.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(%zu overridden)", link->overrides.size());
				ImGui::SameLine();
				if (ImGui::SmallButton("Revert"))
				{
					if (auto* am = context.TryGet<AssetManager>())
					{
						if (auto reloaded = am->LoadMaterialPreset(link->assetPath))
						{
							asset = *reloaded;
							link->overrides.clear();
							changed = true;
						}
					}
				}
				ImGui::SetItemTooltip("Drop this object's changes and take every value from the asset again");
			}
		}

		// Each field is tracked on its own so an override is recorded per field rather than
		// per material - otherwise touching one slider would pin every other value against
		// later asset edits.
		const auto edited = [&](bool fieldChanged, const char* key)
		{
			if (fieldChanged)
			{
				changed = true;
				if (link != nullptr && !link->assetPath.empty())
				{
					link->MarkOverridden(key);
				}
			}
			return fieldChanged;
		};

		edited(PropColor4("Base color", &asset.baseColorFactor.x), "base_color");
		edited(PropSlider("Metallic", &asset.metallicFactor, 0.0f, 1.0f, "%.2f",
		              "0 for anything that is not metal, 1 for bare metal. The values between are for worn edges and blends rather than a dial for shininess - that is Roughness. Metal takes its colour from the base colour and has no diffuse of its own."),
		        "metallic");
		edited(PropSlider("Roughness", &asset.roughnessFactor, 0.0f, 1.0f, "%.2f",
		              "How scattered the reflection is: 0 mirrors its surroundings, 1 diffuses them away entirely. This is what decides whether a highlight reads as a sharp glint or a broad sheen."),
		        "roughness");
		edited(PropSlider("Occlusion", &asset.occlusionStrength, 0.0f, 1.0f, "%.2f",
		              "How far to apply the occlusion texture: 0 ignores it, 1 applies it in full. It does NOTHING without an occlusion map assigned below, and it only darkens ambient light - the sun and local lights are deliberately left alone so it cannot fight the shadow maps."),
		        "occlusion");
		edited(PropColor3("Emissive", &asset.emissiveFactor.x,
		              "Light the surface appears to give off. It is added after shading, so an emissive surface stays bright inside a shadow - but it lights only itself, not anything around it."),
		        "emissive");

		iw::PropLabel("Flags");
		// Wraps onto a second line rather than running off the edge when the panel is narrow.
		const float flagsColumn = ImGui::GetCursorPosX();
		edited(ImGui::Checkbox("Two-sided", &asset.doubleSided), "double_sided");
		iw::ItemTooltip("Draw back faces as well as front. Needed for flat geometry like leaves or cloth that would otherwise vanish from behind, and it costs the culling that would have thrown those faces away.");
		iw::SameLineOrWrap(iw::CheckboxWidth("Blend"), flagsColumn);
		edited(ImGui::Checkbox("Blend", &asset.alphaBlend), "alpha_blend");
		iw::ItemTooltip("True transparency: the surface is mixed with whatever is behind it. Reach for Mask instead when the alpha is really just a cutout - blended surfaces have to be sorted and cannot write depth the way opaque ones do.");
		iw::SameLineOrWrap(iw::CheckboxWidth("Mask"), flagsColumn);
		edited(ImGui::Checkbox("Mask", &asset.alphaMask), "alpha_mask");
		iw::ItemTooltip("Cut the surface out per pixel: anything below Cutoff is discarded outright, everything else stays fully opaque. This is what foliage and chain-link want, not Blend.");
		iw::SameLineOrWrap(iw::CheckboxWidth("Vtx color"), flagsColumn);
		edited(ImGui::Checkbox("Vtx color", &asset.modulateVertexColor), "vertex_color");
		iw::ItemTooltip("Multiply the base colour by the colours stored in the mesh's vertices. Does nothing on a mesh that carries none, which is most of them unless the artist painted them in.");
		edited(PropCheckbox("Receives shadows", &asset.receiveShadows, "When off, the sun never shadows this surface"), "receive_shadows");
		if (asset.alphaMask)
		{
			changed |= PropFloat("Cutoff", &asset.alphaCutoff, 0.01f, 0.0f, 1.0f, "%.2f", "The alpha below which a pixel is thrown away. Only Mask uses it.");
		}

		auto textureRow = [&](const char* label, const char* mapId, TextureHandle& h, TextureColorSpace colorSpace)
		{
			ImGui::PushID(mapId);
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine(iw::LabelWidth());
			if (h.index == TextureHandle::kBrokenIndex)
			{
				ImGui::TextColored(iw::ToImVec4(colors::Mauve), ICON_FA_IMAGE "  missing (magenta fallback)");
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
							h = assets->GetTextureRegistry().Acquire(file->path, colorSpace);
							transientTextureRefs.push_back(h);
							changed = true;
							if (assetDb != nullptr)
							{
								assetDb->Register(MakeTextureSource(file->path));
							}
						}
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (assetDb != nullptr)
			{
				ImGui::SameLine();
				const bool pickTexture = ImGui::SmallButton(ICON_FA_FOLDER_OPEN);
				ImGui::SetItemTooltip("Pick a texture from the project");
				if (pickTexture)
				{
					ImGui::OpenPopup("texpick");
				}
				AssetId pk{};
				if (ImGui::BeginPopup("texpick"))
				{
					bool any = false;
					assetDb->ForEach(AssetType::Texture,
					        [&](AssetId id, const AssetDatabase::Entry& entry)
					        {
						        any = true;
						        if (ImGui::Selectable((entry.displayName + "##" + id.ToHex()).c_str()))
						        {
							        pk = id;
						        }
					        });
					if (!any)
					{
						ImGui::TextDisabled("(drop a texture to add it)");
					}
					if (pk.IsValid())
					{
						ImGui::CloseCurrentPopup();
					}
					ImGui::EndPopup();
				}
				if (pk.IsValid())
				{
					AssetSource src;
					if (assetDb->Describe(pk, src))
					{
						h = assets->GetTextureRegistry().Acquire(src.path, colorSpace);
						transientTextureRefs.push_back(h);
						changed = true;
					}
				}
			}
			ImGui::PopID();
		};
		if (ImGui::TreeNodeEx("Textures", ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			textureRow("Albedo", "albedo", asset.albedoTex, TextureColorSpace::Srgb);
			textureRow("Normal", "normal", asset.normalTex, TextureColorSpace::Linear);
			textureRow("Metal/Rough", "metalrough", asset.metallicRoughnessTex, TextureColorSpace::Linear);
			textureRow("Occlusion", "occlusion", asset.occlusionTex, TextureColorSpace::Linear);
			textureRow("Emissive", "emissive", asset.emissiveTex, TextureColorSpace::Srgb);
			ImGui::TreePop();
		}
		if (changed)
		{
			MaterialSystem::AssignMaterial(world, entity, registry, assets->GetPipelineCache(), asset);
			for (const TextureHandle h: transientTextureRefs)
			{
				if (h.IsValid())
				{
					assets->GetTextureRegistry().Release(h);
				}
			}
		}
		PropText("GPU slot", "%u", mc->gpuSlot);
	}
	void DrawEffectParams(app::LayerContext& context, World& world, Entity entity)
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
			// Capture the effect and its parameters before they go. Nothing recorded this, so
			// Ctrl+Z skipped past it into an unrelated edit and the tuning was gone for good.
			if (auto* undo = context.services.TryGet<UndoStack>())
			{
				const auto* ref = world.TryGet<EffectRefComponent>(entity);
				const auto* params = world.TryGet<EffectParamsComponent>(entity);
				if (ref != nullptr)
				{
					undo->Record(std::make_unique<RemoveEffectCommand>(entity.id, ref->name, params != nullptr ? params->params : EffectParams{}));
				}
			}
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
				world.Remove<PipelineComponent>(entity);
			}
			return;
		}
		if (!open)
		{
			return;
		}
		// Switch between registered effects in place (the Add-Component palette
		// has a single Effect entry; the variant is chosen here).
		if (const auto* er = world.TryGet<EffectRefComponent>(entity))
		{
			auto* sceneCtx = context.TryGet<app::scripting::SceneContext>();
			auto* assets = context.TryGet<AssetManager>();
			if (sceneCtx != nullptr && sceneCtx->effects != nullptr && assets != nullptr)
			{
				std::vector<std::string> names;
				sceneCtx->effects->ForEachEffect([&](const std::string& n, const auto&) { names.push_back(n); });
				std::sort(names.begin(), names.end());
				iw::PropLabel("Effect");
				if (ImGui::BeginCombo("##effectName", er->name.c_str()))
				{
					for (const std::string& n: names)
					{
						if (ImGui::Selectable(n.c_str(), n == er->name) && n != er->name)
						{
							effects::ApplyEntityEffect(world, entity, n, *sceneCtx->effects, assets->GetPipelineCache(), assets->GetEffectParamBuffer());
						}
					}
					ImGui::EndCombo();
				}
			}
			else
			{
				ImGui::TextDisabled("Effect '%s'", er->name.c_str());
			}
		}

		bool changed = false;
		changed |= PropColor4("Tint", &ep->params.tint.x);
		changed |= PropFloat("Speed", &ep->params.speed, 0.02f, 0.0f, 10.0f, "%.2f");
		changed |= PropFloat("Scale", &ep->params.scale, 0.02f, 0.0f, 10.0f, "%.2f");
		changed |= PropFloat("Intensity", &ep->params.intensity, 0.02f, 0.0f, 10.0f, "%.2f");

		if (changed && ep->paramSlot != 0xFFFFFFFFu)
		{
			if (auto* buffer = context.TryGet<EffectParamBuffer>())
			{
				buffer->Write(ep->paramSlot, ep->params);
			}
		}
		PropText("Param slot", "%u", ep->paramSlot);
	}
	namespace
	{
		struct PrimitiveOption
		{
			const char* label;
			const char* kindName;
			PrimitiveMesh kind;
		};

		constexpr PrimitiveOption kPrimitiveOptions[] = {
		        {"Cube", "cube", PrimitiveMesh::Cube},
		        {"Sphere", "sphere", PrimitiveMesh::Sphere},
		        {"Plane", "plane", PrimitiveMesh::Plane},
		        {"Quad", "quad", PrimitiveMesh::Quad},
		        {"Triangle", "triangle", PrimitiveMesh::Triangle},
		};

		std::string PathBasename(const std::string& path)
		{
			const auto slash = path.find_last_of("/\\");
			return slash == std::string::npos ? path : path.substr(slash + 1);
		}

		void EnsureDefaultMaterial(AssetManager& assets, World& world, Entity entity)
		{
			if (world.Has<MaterialComponent>(entity) && world.Has<PipelineComponent>(entity))
			{
				return;
			}
			MaterialAsset asset{};
			asset.baseColorFactor = glm::vec4(0.85f, 0.85f, 0.82f, 1.0f);
			asset.roughnessFactor = 0.6f;
			asset.doubleSided = true;
			MaterialSystem::AssignMaterial(world, entity, assets.GetMaterialRegistry(), assets.GetPipelineCache(), asset);
		}

		AssetId AssetPickerButton(const char* popupId, AssetDatabase* db, AssetType type, AssetId current, const char* emptyLabel, const char* explicitLabel)
		{
			std::string label;
			if (explicitLabel != nullptr)
			{
				label = explicitLabel;
			}
			else if (current.IsValid() && db != nullptr)
			{
				label = db->DisplayName(current);
			}
			if (label.empty())
			{
				label = emptyLabel;
			}
			if (ImGui::Button((label + "##btn" + popupId).c_str(), ImVec2(-FLT_MIN, 0.0f)) && db != nullptr)
			{
				ImGui::OpenPopup(popupId);
			}
			AssetId picked{};
			if (db != nullptr && ImGui::BeginPopup(popupId))
			{
				ImGui::TextDisabled("%s assets", AssetTypeName(type));
				ImGui::Separator();
				bool any = false;
				db->ForEach(type,
				        [&](AssetId id, const AssetDatabase::Entry& entry)
				        {
					        any = true;
					        if (ImGui::Selectable((entry.displayName + "##" + id.ToHex()).c_str(), id == current))
					        {
						        picked = id;
					        }
				        });
				if (!any)
				{
					ImGui::TextDisabled("(none catalogued yet - drop one to add it)");
				}
				if (picked.IsValid())
				{
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
			return picked;
		}
	} // namespace

	void DrawMeshRenderer(app::LayerContext& context, World& world, Entity entity)
	{
		const bool hasMesh = world.Has<MeshComponent>(entity);
		if ((!hasMesh && !world.Has<MeshRendererComponent>(entity)) || world.Has<SpriteRendererComponent>(entity))
		{
			return;
		}
		if (!SectionHeader(ICON_FA_CUBE "  Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen))
		{
			return;
		}

		auto* assets = context.TryGet<AssetManager>();
		auto* primitives = context.TryGet<PrimitiveMeshes>();
		auto* sceneCtx = context.TryGet<app::scripting::SceneContext>();

		const MeshSourceComponent* source = world.TryGet<MeshSourceComponent>(entity);
		bool isModel = source != nullptr && source->kind == MeshSourceComponent::Kind::Model;

		auto* assetDb = context.TryGet<AssetDatabase>();
		const auto refetchSource = [&]()
		{
			source = world.TryGet<MeshSourceComponent>(entity);
			isModel = source != nullptr && source->kind == MeshSourceComponent::Kind::Model;
		};

		const auto applyMeshAsset = [&](AssetId picked)
		{
			AssetSource src;
			if (assetDb == nullptr || !assetDb->Describe(picked, src) || src.type != AssetType::Mesh)
			{
				return;
			}
			if (src.builtin)
			{
				if (primitives == nullptr || assets == nullptr)
				{
					return;
				}
				PrimitiveMesh kind = PrimitiveMesh::Cube;
				for (const PrimitiveOption& opt: kPrimitiveOptions)
				{
					if (src.path == opt.kindName)
					{
						kind = opt.kind;
					}
				}
				world.EmplaceOrReplace<MeshComponent>(entity, MeshComponent{.mesh = &primitives->Get(kind)});
				world.EmplaceOrReplace<MeshSourceComponent>(entity, MeshSourceComponent{.kind = MeshSourceComponent::Kind::Primitive, .path = src.path, .primitiveIndex = 0});
				if (world.Has<SkinnedMeshComponent>(entity))
				{
					world.Remove<SkinnedMeshComponent>(entity);
				}
				EnsureDefaultMaterial(*assets, world, entity);
			}
			else if (assets != nullptr && sceneCtx != nullptr)
			{
				app::scene::AssignModelMeshToEntity(world, *assets, *sceneCtx, entity, src.path, src.subIndex);
			}
		};

		AssetId currentId{};
		if (source != nullptr && assetDb != nullptr)
		{
			if (isModel && assets != nullptr && sceneCtx != nullptr)
			{
				app::scene::RegisterModelAssets(*assetDb, *assets, *sceneCtx, source->path);
			}
			currentId = assetDb->Register(isModel ? MakeModelMeshSource(source->path, static_cast<int>(source->primitiveIndex)) : MakePrimitiveMeshSource(source->path));
		}

		iw::PropLabel("Mesh");
		if (const AssetId picked = AssetPickerButton("##meshAssetPicker", assetDb, AssetType::Mesh, currentId, "None"); picked.IsValid())
		{
			applyMeshAsset(picked);
		}
		iw::ItemTooltip("Pick a mesh asset from the project");
		refetchSource();

		iw::PropLabel("Model");
		const std::string modelLabel = isModel ? PathBasename(source->path) : "Drop .gltf / .glb here";
		ImGui::Button((modelLabel + "##modelSlot").c_str(), ImVec2(-FLT_MIN, 0.0f));
		if (ImGui::BeginDragDropTarget())
		{
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload))
			{
				if (payload->DataSize == sizeof(dragdrop::FilePayload))
				{
					const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
					if (file->kind == dragdrop::FileKind::Model && assets != nullptr && sceneCtx != nullptr)
					{
						if (const auto* project = context.TryGet<app::EditorProjectContext>())
						{
							std::string bakeError;
							if (!editor::EnsureModelBaked(file->path, *project, bakeError))
							{
								AE_WARN(LogCategory::App, "Model import failed for '{}': {}", file->path, bakeError);
							}
						}
						app::scene::AssignModelToEntity(world, *assets, *sceneCtx, entity, file->path);
						if (assetDb != nullptr)
						{
							app::scene::RegisterModelAssets(*assetDb, *assets, *sceneCtx, file->path);
						}
					}
				}
			}
			ImGui::EndDragDropTarget();
		}
		iw::ItemTooltip("Drag a model file from the File Explorer onto this slot");
		refetchSource();

		if (isModel && assets != nullptr && sceneCtx != nullptr && assetDb != nullptr)
		{
			iw::PropLabel("Source");
			if (AccentButton(ICON_FA_ROTATE "  Reload from disk", ImVec2(-FLT_MIN, 0.0f)))
			{
				app::scene::ReloadModelAssets(*assetDb, *assets, *sceneCtx, source->path);
			}
			iw::ItemTooltip("Re-read the model file; the resolve pass re-points meshes next frame (hot-reload)");
		}

		if (isModel && assets != nullptr && sceneCtx != nullptr)
		{
			const int primCount = app::scene::ModelPrimitiveCount(*assets, *sceneCtx, source->path);
			if (primCount > 1)
			{
				int primIndex = static_cast<int>(source->primitiveIndex);
				if (PropInt("Primitive", &primIndex, 0.1f, 0, primCount - 1, "Which mesh of the model this renderer references"))
				{
					app::scene::AssignModelMeshToEntity(world, *assets, *sceneCtx, entity, source->path, primIndex);
				}
			}
			else if (primCount > 0)
			{
				PropText("Primitive", "1 / 1");
			}
		}

		auto* mr = world.TryGet<MeshRendererComponent>(entity);
		bool visible = mr == nullptr || mr->visible;
		bool castShadows = mr == nullptr || mr->castShadows;
		const bool visChanged = PropCheckbox("Visible", &visible, "Hide the mesh without disabling scripts/physics");
		const bool castChanged = PropCheckbox("Cast shadows", &castShadows, "Render this mesh into the shadow maps");
		if (visChanged || castChanged)
		{
			if (mr == nullptr)
			{
				mr = &world.Emplace<MeshRendererComponent>(entity);
			}
			mr->visible = visible;
			mr->castShadows = castShadows;
		}
		ImGui::TextDisabled("Receive shadows is a material flag (Material section)");

		PropText("Material", "%s", world.Has<MaterialComponent>(entity) ? "assigned  (edit in Material)" : "none");
		PropText("Pipeline", "%s", world.Has<PipelineComponent>(entity) ? "present" : "none");
		if (!world.Has<MaterialComponent>(entity) && assets != nullptr && AccentButton(ICON_FA_PALETTE "  Add default material"))
		{
			EnsureDefaultMaterial(*assets, world, entity);
		}
	}

	void DrawSpriteRenderer(app::LayerContext& context, World& world, Entity entity)
	{
		if (!world.Has<SpriteRendererComponent>(entity))
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_IMAGE "  Sprite Renderer", ICON_FA_XMARK "##removeSprite", removed, ImGuiTreeNodeFlags_DefaultOpen, MenuFor(context, world, entity, "Sprite Renderer"));
		if (removed)
		{
			RecordComponentRemoval(context, world, entity, "Sprite Renderer");
			world.Remove<SpriteRendererComponent>(entity);
			return;
		}
		if (!open)
		{
			return;
		}

		auto& sprite = world.Get<SpriteRendererComponent>(entity);
		auto* assetDb = context.TryGet<AssetDatabase>();
		auto* store = context.TryGet<SpriteAssetStore>();
		const auto applyRegion = [&](const SpriteAtlasAsset& atlas, const SpriteRegion& region)
		{
			sprite.texturePath = atlas.texturePath;
			sprite.spriteId = region.id;
			sprite.uvRect = region.uvRect;
			sprite.pixelSize = region.pixelSize;
			sprite.pivot = region.pivot;
			sprite.pixelsPerUnit = atlas.pixelsPerUnit;
		};

		ImGui::SeparatorText("Source");
		if (const auto change = spriteui::DrawAssetSlot(context, "spriteTexture", ICON_FA_IMAGE, "Texture", sprite.texturePath, spriteui::AssetRole::Texture, "Choose or drop a texture", true); change.changed)
		{
			sprite.texturePath = change.path;
			sprite.atlasPath.clear();
			sprite.spriteId = {};
			if (assetDb != nullptr && !change.path.empty())
			{
				assetDb->Register(MakeTextureSource(change.path));
			}
		}
		if (const auto change = spriteui::DrawAssetSlot(context, "spriteAtlas", ICON_FA_IMAGE, "Atlas", sprite.atlasPath, spriteui::AssetRole::Atlas, "Optional sprite atlas", true); change.changed)
		{
			sprite.atlasPath = change.path;
			sprite.spriteId = {};
			if (store != nullptr && !change.path.empty())
			{
				if (const auto atlasResult = store->LoadAtlas(change.path); atlasResult.has_value() && !(**atlasResult).sprites.empty())
				{
					applyRegion(**atlasResult, (**atlasResult).sprites.front());
				}
			}
		}

		if (store != nullptr && !sprite.atlasPath.empty())
		{
			if (const auto atlasResult = store->LoadAtlas(sprite.atlasPath); atlasResult.has_value())
			{
				const SpriteAtlasAsset& atlas = **atlasResult;
				const SpriteRegion* selectedRegion = atlas.Find(sprite.spriteId);
				static char regionFilter[96]{};
				iw::PropLabel("Region");
				const char* regionLabel = selectedRegion != nullptr ? selectedRegion->name.c_str() : "Choose a region";
				if (ImGui::Button(regionLabel, ImVec2(-FLT_MIN, 0.0f)))
				{
					regionFilter[0] = '\0';
					ImGui::OpenPopup("##spriteRegionPicker");
				}
				if (ImGui::BeginPopup("##spriteRegionPicker"))
				{
					ImGui::SetNextItemWidth(280.0f);
					ImGui::InputTextWithHint("##regionFilter", "Search regions...", regionFilter, sizeof(regionFilter));
					ImGui::Separator();
					const std::string filter = spriteui::Lower(regionFilter);
					bool any = false;
					for (const SpriteRegion& region: atlas.sprites)
					{
						if (!filter.empty() && !spriteui::Lower(region.name).contains(filter))
						{
							continue;
						}
						any = true;
						if (ImGui::Selectable(region.name.c_str(), region.id == sprite.spriteId))
						{
							applyRegion(atlas, region);
							ImGui::CloseCurrentPopup();
						}
					}
					if (!any)
					{
						ImGui::TextDisabled("No matching regions.");
					}
					ImGui::EndPopup();
				}
				iw::ItemTooltip("Click to search atlas regions by name");
			}
		}

		ImGui::SeparatorText("Appearance");
		PropColor4("Tint", &sprite.tint.x);
		int blend = static_cast<int>(sprite.blendMode);
		constexpr const char* kBlendNames[] = {"Alpha", "Additive", "Multiply", "Opaque"};
		if (PropCombo("Blend Mode", &blend, kBlendNames, static_cast<int>(std::size(kBlendNames))))
		{
			sprite.blendMode = static_cast<SpriteBlendMode>(blend);
		}
		PropCheckbox("Visible", &sprite.visible, "Hide this sprite without disabling the entity");
		PropCheckbox("Flip X", &sprite.flipX);
		PropCheckbox("Flip Y", &sprite.flipY);
		PropCheckbox("Pixel Snap", &sprite.pixelSnap, "Snap sprite placement to the pixel grid");
		PropCheckbox("Pixel Art", &sprite.pixelArt, "Sample the sprite with nearest filtering for crisp pixel-art edges");

		if (ImGui::TreeNodeEx("Layout & Sorting", ImGuiTreeNodeFlags_SpanAvailWidth))
		{
			PropDrag2("Pixel Size", &sprite.pixelSize.x, 1.0f, 1.0f, 16384.0f, "%.0f");
			PropDrag2("Pivot", &sprite.pivot.x, 0.01f, 0.0f, 1.0f);
			PropFloat("Pixels Per Unit", &sprite.pixelsPerUnit, 1.0f, 0.001f, 10000.0f, "%.1f");
			PropInt("Sorting Layer", &sprite.sortingLayer, 1.0f, 0, 0, FieldTip("Sprite Renderer", "sorting_layer"));
			PropInt("Order In Layer", &sprite.orderInLayer, 1.0f, 0, 0, FieldTip("Sprite Renderer", "order_in_layer"));
			ImGui::TreePop();
		}
	}

	void DrawSpriteAnimator(app::LayerContext& context, World& world, Entity entity)
	{
		if (!world.Has<SpriteAnimatorComponent>(entity))
		{
			return;
		}
		bool removed = false;
		const bool open = RemovableSection(ICON_FA_FILM "  Sprite Animator", ICON_FA_XMARK "##removeSpriteAnimator", removed, ImGuiTreeNodeFlags_DefaultOpen, MenuFor(context, world, entity, "Sprite Animator"));
		if (removed)
		{
			RecordComponentRemoval(context, world, entity, "Sprite Animator");
			world.Remove<SpriteAnimatorComponent>(entity);
			return;
		}
		if (!open)
		{
			return;
		}

		auto& animator = world.Get<SpriteAnimatorComponent>(entity);
		auto* store = context.TryGet<SpriteAssetStore>();
		const auto resetRuntime = [&](bool play)
		{
			animator.currentFrame = animator.startFrame;
			animator.frameTime = 0.0f;
			animator.fixedAccumulator = 0.0f;
			animator.direction = 1;
			animator.initialized = true;
			animator.playing = play;
		};

		ImGui::SeparatorText("Animation Clip");
		if (const auto change = spriteui::DrawAssetSlot(context, "spriteAnimation", ICON_FA_FILM, "Clip", animator.animationPath, spriteui::AssetRole::Animation, "Choose or drop an animation", true); change.changed)
		{
			animator.animationPath = change.path;
			animator.initialized = false;
			if (!world.Has<TransformComponent>(entity))
			{
				world.Emplace<TransformComponent>(entity);
			}
			if (!world.Has<SpriteRendererComponent>(entity))
			{
				world.Emplace<SpriteRendererComponent>(entity);
			}
		}

		const SpriteAnimationAsset* clip = nullptr;
		if (store != nullptr && !animator.animationPath.empty())
		{
			if (const auto result = store->LoadAnimation(animator.animationPath); result.has_value())
			{
				clip = *result;
			}
		}
		if (clip != nullptr)
		{
			PropText("Clip Info", "%zu frames  -  %.2f seconds", clip->frames.size(), clip->DurationSeconds());
		}
		else if (!animator.animationPath.empty())
		{
			ImGui::TextColored(chrome::kError, ICON_FA_CIRCLE_INFO "  Animation clip could not be loaded.");
		}

		const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (chrome::OutlineButton(animator.playing ? ICON_FA_STOP "  Pause Preview" : ICON_FA_PLAY "  Preview", ImVec2(buttonWidth, 0.0f)))
		{
			if (!animator.initialized)
			{
				resetRuntime(true);
			}
			else
			{
				animator.playing = !animator.playing;
			}
		}
		ImGui::SameLine();
		if (chrome::OutlineButton(ICON_FA_ROTATE "  Restart", ImVec2(buttonWidth, 0.0f)))
		{
			resetRuntime(true);
		}

		ImGui::SeparatorText("Playback");
		PropCheckbox("Autoplay", &animator.autoplay, "Start this animation automatically when the scene loads");
		PropFloat("Speed", &animator.speed, 0.05f, 0.0f, 20.0f, "%.2fx");
		int startFrame = static_cast<int>(animator.startFrame);
		const int maxFrame = clip != nullptr && !clip->frames.empty() ? static_cast<int>(clip->frames.size() - 1) : 0;
		if (PropInt("Start Frame", &startFrame, 1.0f, 0, maxFrame))
		{
			animator.startFrame = static_cast<std::uint32_t>(std::clamp(startFrame, 0, maxFrame));
			animator.initialized = false;
		}
		PropCheckbox("Use Clip Loop", &animator.useAssetLoopMode, "Use the loop mode saved in the animation clip");
		if (!animator.useAssetLoopMode)
		{
			int loopMode = static_cast<int>(animator.loopMode);
			constexpr const char* kLoopModes[] = {"Loop", "Once", "Ping Pong", "Hold"};
			if (PropCombo("Loop Mode", &loopMode, kLoopModes, static_cast<int>(std::size(kLoopModes))))
			{
				animator.loopMode = static_cast<SpriteAnimationLoopMode>(loopMode);
			}
		}
		if (clip != nullptr && !clip->frames.empty())
		{
			PropText("Preview Frame", "%u / %zu", std::min<std::uint32_t>(animator.currentFrame + 1u, static_cast<std::uint32_t>(clip->frames.size())), clip->frames.size());
		}
	}
} // namespace aether::editor
