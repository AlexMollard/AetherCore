#include "debug/MaterialAssetEditor.hpp"

#include <cfloat>
#include <filesystem>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

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
				// A file that does not parse yields a DEFAULT spec, and saving that back is
				// exactly the "unreadable material into an overwritten one" this guard exists
				// to prevent - the read check above only caught a file that could not be read
				// at all.
				bool parsed = false;
				state.spec = MaterialSerializer::Parse(assetPath, *text, &parsed);
				if (!parsed)
				{
					state.error = "This material is not readable TOML, so editing it here would overwrite it with defaults. Fix the file first.";
					state.spec = {};
					state.loaded = false;
				}
				else
				{
					state.saved = state.spec;
					state.loaded = true;
				}
			}
		}

		if (!state.error.empty())
		{
			ImGui::TextColored(chrome::kWarning, "%s", state.error.c_str());
			return;
		}

		MaterialAsset& m = state.spec.material;
		bool changed = false;

		// ImGui puts a widget's label to its RIGHT, which in an inspector column this narrow
		// truncates every one of them to "Roughness..." or "Receives s". A two-column table
		// puts the labels on the left where they fit and gives every control the same width,
		// which is also what makes the column read as a list rather than as a pile.
		const auto row = [](const char* label, const char* tip = nullptr)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label);
			if (tip != nullptr)
			{
				ImGui::SetItemTooltip("%s", tip);
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
		};

		// A colour that fills its column instead of a stamp-sized square stranded beside a
		// lot of empty space. ColorEdit with NoInputs ignores the item width and always draws
		// one frame-height square, so the swatch and its picker are built here instead.
		const auto colourRow = [&](const char* id, float* rgba, bool hasAlpha, bool hdr)
		{
			const ImVec4 shown(rgba[0], rgba[1], rgba[2], hasAlpha ? rgba[3] : 1.0f);
			ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaPreviewHalf;
			if (hdr)
			{
				flags |= ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;
			}
			if (!hasAlpha)
			{
				flags |= ImGuiColorEditFlags_NoAlpha;
			}
			const ImVec2 swatch(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
			if (ImGui::ColorButton(id, shown, flags, swatch))
			{
				ImGui::OpenPopup(id);
			}
			ImGui::SetItemTooltip("Click to pick");
			bool edited = false;
			if (ImGui::BeginPopup(id))
			{
				edited = hasAlpha ? ImGui::ColorPicker4("##pick", rgba, flags | ImGuiColorEditFlags_AlphaBar)
				                  : ImGui::ColorPicker3("##pick", rgba, flags);
				ImGui::EndPopup();
			}
			return edited;
		};

		constexpr ImGuiTableFlags kFieldTable = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
		ImGui::SeparatorText("Surface");
		if (ImGui::BeginTable("##surface", 2, kFieldTable))
		{
			// A fixed label column: proportional sizing shrank the labels away again as soon
			// as the panel narrowed, which is the whole problem.
			ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);

			row("Base colour");
			changed |= colourRow("##base", &m.baseColorFactor.x, /*hasAlpha*/ true, /*hdr*/ false);

			row("Metallic", "0 for everything except bare metal.");
			changed |= ImGui::SliderFloat("##metallic", &m.metallicFactor, 0.0f, 1.0f, "%.2f");

			row("Roughness", "0 is a mirror, 1 is chalk.");
			changed |= ImGui::SliderFloat("##roughness", &m.roughnessFactor, 0.0f, 1.0f, "%.2f");

			row("Occlusion", "How strongly the occlusion texture darkens ambient light.");
			changed |= ImGui::SliderFloat("##occlusion", &m.occlusionStrength, 0.0f, 1.0f, "%.2f");

			row("Emissive", "Light this surface gives off. Not affected by lighting.");
			changed |= colourRow("##emissive", &m.emissiveFactor.x, /*hasAlpha*/ false, /*hdr*/ true);

			if (m.alphaMask)
			{
				row("Cutoff", "Pixels below this alpha are discarded.");
				changed |= ImGui::SliderFloat("##cutoff", &m.alphaCutoff, 0.0f, 1.0f, "%.2f");
			}
			ImGui::EndTable();
		}

		ImGui::SeparatorText("Options");
		if (ImGui::BeginTable("##options", 2, ImGuiTableFlags_SizingStretchSame))
		{
			const auto toggle = [&](const char* label, bool* value, const char* tip)
			{
				ImGui::TableNextColumn();
				changed |= ImGui::Checkbox(label, value);
				ImGui::SetItemTooltip("%s", tip);
			};
			toggle("Two-sided", &m.doubleSided, "Draw back faces as well as front faces.");
			toggle("Blend", &m.alphaBlend, "Blend with what is behind, using the base colour's alpha.");
			toggle("Alpha mask", &m.alphaMask, "Discard pixels below the cutoff instead of blending.");
			toggle("Shadows", &m.receiveShadows, "Receive shadows cast by other objects.");
			ImGui::EndTable();
		}
		// The longest label, so it gets the full width rather than being clipped to
		// "Vertex colou" in a half-width cell.
		changed |= ImGui::Checkbox("Vertex colour", &m.modulateVertexColor);
		ImGui::SetItemTooltip("Multiply the base colour by the mesh's own vertex colours.");

		ImGui::SeparatorText("Textures");
		const std::pair<const char*, std::string*> slots[] = {
		        {"Albedo", &state.spec.albedoPath},
		        {"Normal", &state.spec.normalPath},
		        {"Metal/Rough", &state.spec.metallicRoughnessPath},
		        {"Occlusion", &state.spec.occlusionPath},
		        {"Emissive", &state.spec.emissivePath},
		};
		if (ImGui::BeginTable("##textures", 3, ImGuiTableFlags_SizingFixedFit))
		{
			ImGui::TableSetupColumn("##slot", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			ImGui::TableSetupColumn("##path", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##clear", ImGuiTableColumnFlags_WidthFixed);
			for (const auto& [label, path]: slots)
			{
				ImGui::PushID(label);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(label);

				ImGui::TableSetColumnIndex(1);
				const bool assigned = !path->empty();
				const std::string name = assigned ? std::filesystem::path(*path).filename().generic_string() : std::string("Empty");
				// A framed slot the width of the cell, so an empty one still looks like
				// somewhere a texture goes and the whole row is the drop target - not just
				// the few pixels the filename happens to cover.
				const ImVec2 slotSize(-FLT_MIN, 0.0f);
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(assigned ? ImGuiCol_Text : ImGuiCol_TextDisabled));
				ImGui::ButtonEx(name.c_str(), slotSize, ImGuiButtonFlags_AlignTextBaseLine);
				ImGui::PopStyleColor(2);
				ImGui::SetItemTooltip("%s", assigned ? path->c_str() : "Drag a texture from the File Explorer onto this slot.");
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

				ImGui::TableSetColumnIndex(2);
				ImGui::BeginDisabled(!assigned);
				if (ImGui::SmallButton(ICON_FA_XMARK))
				{
					path->clear();
					changed = true;
				}
				ImGui::EndDisabled();
				if (assigned)
				{
					ImGui::SetItemTooltip("Clear this slot");
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		ImGui::Spacing();

		// `changed` only tells us a widget reported an edit; whether anything actually moved
		// is a comparison against what is on disk. That is also what drives the preview, so a
		// drag updates it continuously rather than only on mouse-up.
		(void) changed;
		state.dirty = !MaterialSpecEquals(state.spec, state.saved);

		ImGui::Separator();

		// Written only when asked. Saving on every mouse-up rewrites the asset - and with it
		// every object linked to the asset - for a slider you were only auditioning, and
		// there is no undo for a file.
		ImGui::BeginDisabled(!state.dirty || !state.loaded);
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
