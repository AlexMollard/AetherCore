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

		// Built from the IN-MEMORY spec, not reloaded from the file: the point of the preview
		// is to show the values you are dragging right now, and those are not on disk until
		// you press Save.
		MaterialAsset material = m_edit.spec.material;

		// A material may name its own shader, and the preview has to honour it or it shows
		// the standard lighting for a surface that will not render with it. Interned because
		// MaterialTemplate holds the path as a view the pipeline cache outlives.
		if (!m_edit.spec.shaderVfsPath.empty())
		{
			material.templateDesc.shaderVfsPath = assets->InternShaderVfsPath(m_edit.spec.shaderVfsPath);
		}

		auto& textures = assets->GetTextureRegistry();
		const auto acquire = [&textures](const std::string& path, TextureHandle& out, TextureColorSpace colorSpace)
		{
			out = path.empty() ? TextureHandle{} : textures.Acquire(path, colorSpace);
		};
		// Base colour and emissive carry light and decode as sRGB; the rest are data.
		acquire(m_edit.spec.albedoPath, material.albedoTex, TextureColorSpace::Srgb);
		acquire(m_edit.spec.normalPath, material.normalTex, TextureColorSpace::Linear);
		acquire(m_edit.spec.metallicRoughnessPath, material.metallicRoughnessTex, TextureColorSpace::Linear);
		acquire(m_edit.spec.occlusionPath, material.occlusionTex, TextureColorSpace::Linear);
		acquire(m_edit.spec.emissivePath, material.emissiveTex, TextureColorSpace::Srgb);

		std::string error;
		if (!rendering->GetModelPreview().ShowMaterialOnMesh(*assets, primitives->Get(PrimitiveMesh::Sphere), material, error))
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

		for (const TextureHandle h: {material.albedoTex, material.normalTex, material.metallicRoughnessTex, material.occlusionTex, material.emissiveTex})
		{
			if (h.IsValid())
			{
				textures.Release(h);
			}
		}
		m_previewSpec = m_edit.spec;
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

		// Refreshed whenever the values differ from what the preview was last built with, so
		// dragging a slider updates it continuously instead of only on release.
		if (m_previewDirty || !MaterialSpecEquals(m_edit.spec, m_previewSpec))
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

		DrawMaterialAssetEditor(context, world, m_edit.path, m_edit);

		ImGui::End();
	}
} // namespace aether::editor
