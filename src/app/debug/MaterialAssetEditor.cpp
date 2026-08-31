#include "debug/MaterialAssetEditor.hpp"

#include <filesystem>
#include <utility>
#include <vector>

#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/EditorDragDrop.hpp"
#include "io/FileSystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/MaterialSystem.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	namespace
	{
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

	// Same field mapping the scene serde uses, keyed by the scene TOML names so the
	// component, the file and both copiers agree on what a field is called.
	void ApplyMaterialOverridesTo(MaterialAsset& onto, const MaterialAsset& from, const std::vector<std::string>& keys)
	{
		for (const std::string& key: keys)
		{
			if (key == "base_color") { onto.baseColorFactor = from.baseColorFactor; }
			else if (key == "metallic") { onto.metallicFactor = from.metallicFactor; }
			else if (key == "roughness") { onto.roughnessFactor = from.roughnessFactor; }
			else if (key == "occlusion") { onto.occlusionStrength = from.occlusionStrength; }
			else if (key == "alpha_cutoff") { onto.alphaCutoff = from.alphaCutoff; }
			else if (key == "emissive") { onto.emissiveFactor = from.emissiveFactor; }
			else if (key == "double_sided") { onto.doubleSided = from.doubleSided; }
			else if (key == "alpha_blend") { onto.alphaBlend = from.alphaBlend; }
			else if (key == "alpha_mask") { onto.alphaMask = from.alphaMask; }
			else if (key == "vertex_color") { onto.modulateVertexColor = from.modulateVertexColor; }
			else if (key == "receive_shadows") { onto.receiveShadows = from.receiveShadows; }
		}
	}

	} // namespace

	bool MaterialSpecEquals(const MaterialPresetSpec& a, const MaterialPresetSpec& b)
	{
		const MaterialAsset& x = a.material;
		const MaterialAsset& y = b.material;
		return x.baseColorFactor == y.baseColorFactor && x.emissiveFactor == y.emissiveFactor
		        && x.metallicFactor == y.metallicFactor && x.roughnessFactor == y.roughnessFactor
		        && x.occlusionStrength == y.occlusionStrength && x.alphaCutoff == y.alphaCutoff
		        && x.doubleSided == y.doubleSided && x.alphaBlend == y.alphaBlend && x.alphaMask == y.alphaMask
		        && x.modulateVertexColor == y.modulateVertexColor && x.receiveShadows == y.receiveShadows
		        && a.albedoPath == b.albedoPath && a.normalPath == b.normalPath
		        && a.metallicRoughnessPath == b.metallicRoughnessPath && a.occlusionPath == b.occlusionPath
		        && a.emissivePath == b.emissivePath && a.shaderVfsPath == b.shaderVfsPath;
	}


	// Push a just-saved material asset into every entity linked to it, leaving each
	// entity's own overridden fields alone. This is the half of "linked" that you can
	// actually see: edit the asset, and the objects using it change now rather than on
	// the next scene load.
	int PropagateMaterialAsset(app::LayerContext& context, World& world, const std::string& assetPath)
	{
		auto* assets = context.TryGet<AssetManager>();
		if (assets == nullptr)
		{
			return 0;
		}
		auto loaded = assets->LoadMaterialPreset(assetPath);
		if (!loaded)
		{
			return 0;
		}

		std::vector<std::pair<Entity, MaterialAsset>> pending;
		for (const auto& [enttE, link]: world.GetRegistry().view<MaterialLinkComponent>().each())
		{
			if (link.assetPath != assetPath)
			{
				continue;
			}
			const Entity entity = World::FromEntt(enttE);
			MaterialAsset merged = *loaded;
			if (const auto* inst = world.TryGet<MaterialInstanceComponent>(entity))
			{
				// Re-applying the asset must not undo what this entity overrode.
				ApplyMaterialOverridesTo(merged, inst->asset, link.overrides);
			}
			pending.emplace_back(entity, merged);
		}

		// Collected first: AssignMaterial touches components the view is iterating.
		for (const auto& [entity, material]: pending)
		{
			MaterialSystem::AssignMaterial(world, entity, assets->GetMaterialRegistry(), assets->GetPipelineCache(), material);
		}
		ReleaseMaterialAssetTextures(*assets, *loaded);
		return static_cast<int>(pending.size());
	}


	// Edits the .material.toml itself, rather than a copy living on some entity. This is
	// what makes a material an asset: change it here and every object that references it
	// changes, instead of you re-applying a preset object by object.
	void DrawMaterialAssetEditor(app::LayerContext& context, World& world, const std::string& assetPath, MaterialAssetEditState& state)
	{
		if (!state.loaded || state.path != assetPath)
		{
			state.path = assetPath;
			state.loaded = false;
			state.dirty = false;
			state.error.clear();
			auto text = io::FileSystem::ReadFileText(assetPath);
			if (!text)
			{
				// Never offer to save over a file we could not read - that turns an
				// unreadable material into an overwritten one.
				state.error = "Could not read this material.";
				state.spec = {};
			}
			else
			{
				state.spec = MaterialSerializer::Parse(assetPath, *text);
				state.saved = state.spec;
				state.loaded = true;
			}
		}

		if (!state.error.empty())
		{
			ImGui::TextColored(chrome::kWarning, "%s", state.error.c_str());
			return;
		}

		MaterialAsset& m = state.spec.material;
		bool changed = false;
		changed |= ImGui::ColorEdit4("Base color", &m.baseColorFactor.x);
		changed |= ImGui::SliderFloat("Metallic", &m.metallicFactor, 0.0f, 1.0f, "%.2f");
		changed |= ImGui::SliderFloat("Roughness", &m.roughnessFactor, 0.0f, 1.0f, "%.2f");
		changed |= ImGui::SliderFloat("Occlusion", &m.occlusionStrength, 0.0f, 1.0f, "%.2f");
		changed |= ImGui::ColorEdit3("Emissive", &m.emissiveFactor.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
		changed |= ImGui::Checkbox("Two-sided", &m.doubleSided);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Blend", &m.alphaBlend);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Mask", &m.alphaMask);
		changed |= ImGui::Checkbox("Vertex colour", &m.modulateVertexColor);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("Receives shadows", &m.receiveShadows);
		if (m.alphaMask)
		{
			changed |= ImGui::SliderFloat("Cutoff", &m.alphaCutoff, 0.0f, 1.0f, "%.2f");
		}

		ImGui::SeparatorText("Textures");
		const std::pair<const char*, std::string*> slots[] = {
		        {"Albedo", &state.spec.albedoPath},
		        {"Normal", &state.spec.normalPath},
		        {"Metallic/Rough", &state.spec.metallicRoughnessPath},
		        {"Occlusion", &state.spec.occlusionPath},
		        {"Emissive", &state.spec.emissivePath},
		};
		for (const auto& [label, path]: slots)
		{
			ImGui::PushID(label);
			ImGui::TextDisabled("%s", label);
			ImGui::SameLine(140.0f);
			ImGui::TextUnformatted(path->empty() ? "(none)  drop a texture here" : path->c_str());
			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(dragdrop::kFilePayload);
				        payload != nullptr && payload->DataSize == sizeof(dragdrop::FilePayload))
				{
					const auto* file = static_cast<const dragdrop::FilePayload*>(payload->Data);
					if (file->kind == dragdrop::FileKind::Texture)
					{
						*path = file->path;
						changed = true;
					}
				}
				ImGui::EndDragDropTarget();
			}
			if (!path->empty())
			{
				ImGui::SameLine();
				if (ImGui::SmallButton(ICON_FA_XMARK))
				{
					path->clear();
					changed = true;
				}
			}
			ImGui::PopID();
		}

		// `changed` only tells us a widget reported an edit; whether anything actually moved
		// is a comparison against what is on disk. That is also what drives the preview, so a
		// drag updates it continuously rather than only on mouse-up.
		(void) changed;
		state.dirty = !MaterialSpecEquals(state.spec, state.saved);

		ImGui::Separator();

		// Written only when asked. Saving on every mouse-up rewrites the asset - and with it
		// every object linked to the asset - for a slider you were only auditioning, and
		// there is no undo for a file.
		ImGui::BeginDisabled(!state.dirty);
		if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save"))
		{
			if (auto written = io::FileSystem::WriteFileText(assetPath, MaterialSerializer::ToToml(state.spec)); !written)
			{
				state.error = "Could not write this material.";
			}
			else
			{
				state.saved = state.spec;
				state.dirty = false;
				state.linkedCount = PropagateMaterialAsset(context, world, assetPath);
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Revert"))
		{
			state.spec = state.saved;
			state.dirty = false;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (state.dirty)
		{
			ImGui::TextColored(chrome::kWarning, "Unsaved changes");
		}
		else if (state.linkedCount > 0)
		{
			ImGui::TextDisabled("Saved - updated %d linked object%s", state.linkedCount, state.linkedCount == 1 ? "" : "s");
		}
		else
		{
			ImGui::TextDisabled("Saved");
		}
	}

} // namespace aether::editor
