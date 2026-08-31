#include "debug/MaterialEditorPanel.hpp"

#include <filesystem>

#include <imgui.h>

#include "assets/AssetManager.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "debug/SceneSelection.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "material/TextureRegistry.hpp"
#include "mesh/PrimitiveMeshes.hpp"
#include "rendering/RenderingSubsystem.hpp"
#include "scene/World.hpp"

namespace aether::editor
{
	void MaterialEditorPanel::RefreshPreview(app::LayerContext& context)
	{
		m_previewDirty = false;
		m_previewImGuiId = 0;
		m_previewError.clear();

		auto* assets = context.TryGet<AssetManager>();
		auto* rendering = context.TryGet<RenderingSubsystem>();
		auto* imgui = context.TryGet<ImguiSubsystem>();
		auto* primitives = context.TryGet<PrimitiveMeshes>();
		if (assets == nullptr || rendering == nullptr || imgui == nullptr || primitives == nullptr)
		{
			m_previewError = "Preview is unavailable in this build.";
			return;
		}

		// Loaded back from the file rather than resolved from the in-memory spec: the spec
		// holds texture PATHS, and going through the loader is the same path the renderer
		// takes, so the preview cannot quietly disagree with the real thing.
		auto loaded = assets->LoadMaterialPreset(m_edit.path);
		if (!loaded)
		{
			m_previewError = "Could not load this material for preview.";
			return;
		}

		std::string error;
		if (!rendering->GetModelPreview().ShowMaterialOnMesh(*assets, primitives->Get(PrimitiveMesh::Sphere), *loaded, error))
		{
			m_previewError = error;
		}
		else
		{
			const ImTextureID id = imgui->RegisterTexture(rendering->GetModelPreview().GetColorView(), gpu::ImageLayout::ShaderReadOnly);
			if (id != ImTextureID_Invalid)
			{
				m_previewImGuiId = static_cast<std::uint64_t>(id);
			}
		}

		auto& textures = assets->GetTextureRegistry();
		for (const TextureHandle h: {loaded->albedoTex, loaded->normalTex, loaded->metallicRoughnessTex, loaded->occlusionTex, loaded->emissiveTex})
		{
			if (h.IsValid())
			{
				textures.Release(h);
			}
		}
	}

	void MaterialEditorPanel::OnImGui(app::LayerContext& context)
	{
		ImGui::Begin("Material Editor", VisiblePtr());

		auto& world = context.Get<World>();
		auto* selection = context.TryGet<SceneSelection>();

		// Follows a material selection, and then KEEPS it: selecting an object afterwards
		// leaves the material open, which is the difference between this window and the
		// Inspector's material view. An imported binary .material is skipped - the next model
		// import would overwrite anything authored into it.
		if (selection != nullptr && selection->HasAsset()
		        && selection->SelectedAsset().kind == SceneSelection::AssetKind::Material
		        && !selection->SelectedAsset().path.ends_with(".material")
		        && selection->SelectedAsset().path != m_edit.path)
		{
			m_edit = MaterialAssetEditState{};
			m_edit.path = selection->SelectedAsset().path;
			m_previewDirty = true;
		}

		if (m_edit.path.empty())
		{
			ImGui::TextDisabled("No material open.");
			ImGui::TextDisabled("Select a .material.toml in the File Explorer and it opens here.");
			ImGui::End();
			return;
		}

		ImGui::TextUnformatted(std::filesystem::path(m_edit.path).filename().generic_string().c_str());
		ImGui::SetItemTooltip("%s", m_edit.path.c_str());
		ImGui::Separator();

		if (m_previewDirty)
		{
			RefreshPreview(context);
		}

		if (m_previewImGuiId != 0)
		{
			const float side = std::min(ImGui::GetContentRegionAvail().x, 220.0f);
			ImGui::Image(static_cast<ImTextureID>(m_previewImGuiId), ImVec2(side, side));
		}
		else if (!m_previewError.empty())
		{
			ImGui::TextColored(chrome::kWarning, "%s", m_previewError.c_str());
		}
		ImGui::Separator();

		// A save is what makes the preview stale, so the refresh is keyed off the dirty flag
		// falling rather than off any edit - otherwise every slider frame would reload the
		// material from disk and re-record a preview pass.
		const bool wasDirty = m_edit.dirty;
		DrawMaterialAssetEditor(context, world, m_edit.path, m_edit);
		if (wasDirty && !m_edit.dirty)
		{
			m_previewDirty = true;
		}

		ImGui::End();
	}
} // namespace aether::editor
