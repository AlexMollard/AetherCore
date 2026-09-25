#include "twinsanity/ReferenceImagesPanel.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "Icons.hpp"
#include "assets/AssetManager.hpp"
#include "editor/EditorProjectContext.hpp"
#include "gpu/ResourceRegistry.hpp"
#include "imgui/ImguiSubsystem.hpp"
#include "layers/AppLayer.hpp"
#include "utils/Logger.hpp"
#include "utils/Profiler.hpp"

namespace aether::editor::twinsanity
{
	namespace
	{
		bool IsPng(const std::filesystem::path& path)
		{
			std::string ext = path.extension().string();
			std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return ext == ".png";
		}
	} // namespace

	void ReferenceImagesPanel::OnDetach(app::LayerContext& context)
	{
		ReleasePreview(context);
		m_entries.clear();
		m_scannedRoot.clear();
		m_stampValid = false;
		m_selected = -1;
	}

	void ReferenceImagesPanel::Scan(const std::filesystem::path& referenceDir)
	{
		m_entries.clear();
		std::error_code ec;
		if (!std::filesystem::is_directory(referenceDir, ec))
		{
			return;
		}
		for (const auto& entry: std::filesystem::directory_iterator(referenceDir, ec))
		{
			if (!entry.is_regular_file() || !IsPng(entry.path()))
			{
				continue;
			}
			m_entries.push_back({entry.path(), entry.path().stem().string()});
		}
		std::ranges::sort(m_entries, [](const Entry& a, const Entry& b) { return a.name < b.name; });
	}

	void ReferenceImagesPanel::ReleasePreview(app::LayerContext& context)
	{
		if (m_previewImguiId != 0)
		{
			if (auto* imgui = context.TryGet<ImguiSubsystem>())
			{
				imgui->UnregisterTexture(static_cast<ImTextureID>(m_previewImguiId));
			}
			m_previewImguiId = 0;
		}
		m_previewTexture.Destroy();
		m_previewIndex = -1;
		m_previewWidth = 0;
		m_previewHeight = 0;
	}

	bool ReferenceImagesPanel::LoadPreview(app::LayerContext& context, const int index)
	{
		if (index == m_previewIndex && m_previewImguiId != 0)
		{
			return true;
		}
		if (index < 0 || index >= static_cast<int>(m_entries.size()))
		{
			return false;
		}
		auto* assets = context.TryGet<AssetManager>();
		auto* imgui = context.TryGet<ImguiSubsystem>();
		if (assets == nullptr || imgui == nullptr)
		{
			m_error = "Texture services are not available.";
			return false;
		}
		auto texture = assets->CreateTextureFromDisk(m_entries[static_cast<std::size_t>(index)].path);
		if (!texture)
		{
			m_error = texture.error().message;
			AE_WARN(LogCategory::App, "Reference image '{}' failed to load: {}", m_entries[static_cast<std::size_t>(index)].path.string(), m_error);
			ReleasePreview(context);
			return false;
		}
		ReleasePreview(context);
		const ImTextureID id = imgui->RegisterTexture(texture->GetView(), gpu::ImageLayout::ShaderReadOnly);
		if (id == ImTextureID_Invalid)
		{
			m_error = "Could not register the image with ImGui.";
			return false;
		}
		m_previewTexture = std::move(*texture);
		m_previewImguiId = static_cast<std::uint64_t>(id);
		m_previewIndex = index;
		// Extent lookup follows the File Explorer thumbnail path: match the freshly bound
		// bindless slot in the registry, which avoids a second decode of the file.
		m_previewWidth = 0;
		m_previewHeight = 0;
		const std::uint32_t slot = m_previewTexture.GetBindlessSlot();
		for (const gpu::DebugTextureInfo& info: gpu::ResourceRegistry::ListDebugTextures())
		{
			if (info.hasBindlessSampled && info.bindlessSampledSlot == slot)
			{
				m_previewWidth = static_cast<int>(info.extent.width);
				m_previewHeight = static_cast<int>(info.extent.height);
				break;
			}
		}
		m_error.clear();
		return true;
	}

	void ReferenceImagesPanel::OnImGui(app::LayerContext& context)
	{
		AE_PROFILE_ZONE();
		// Its own window, closable from the title bar and the Window menu. Drawing without
		// one put everything into ImGui's implicit fallback window ("Debug"), which has no
		// close button and ignored the panel's visibility.
		if (ImGui::Begin(GetName().data(), VisiblePtr()))
		{
			DrawContents(context);
		}
		ImGui::End();
	}

	void ReferenceImagesPanel::DrawContents(app::LayerContext& context)
	{
		const auto* project = context.TryGet<app::EditorProjectContext>();
		if (project == nullptr || !project->IsLoaded())
		{
			ImGui::TextDisabled("Open a project to browse its reference images.");
			return;
		}

		const std::filesystem::path referenceDir = app::ResolveProjectPath(project, "reference");
		// Captures land while the editor is open, so the scan can't run only at project
		// switch: stat the folder once per frame and rescan when its write time moves.
		// A rescan keeps the selection by name rather than by index.
		std::error_code timeEc;
		const auto referenceStamp = std::filesystem::last_write_time(referenceDir, timeEc);
		const bool stampKnown = !timeEc;
		if (m_scannedRoot != project->root || (stampKnown && (!m_stampValid || referenceStamp != m_referenceStamp)))
		{
			m_scannedRoot = project->root;
			m_stampValid = stampKnown;
			m_referenceStamp = stampKnown ? referenceStamp : m_referenceStamp;
			const std::string previousName = m_selected >= 0 && m_selected < static_cast<int>(m_entries.size()) ? m_entries[static_cast<std::size_t>(m_selected)].name : std::string{};
			Scan(referenceDir);
			m_selected = -1;
			m_error.clear();
			for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
			{
				if (m_entries[static_cast<std::size_t>(i)].name == previousName)
				{
					m_selected = i;
					break;
				}
			}
		}

		if (m_entries.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::kMuted);
			ImGui::TextWrapped("%s", ICON_FA_IMAGE "  No reference images yet.");
			ImGui::PopStyleColor();
			ImGui::Spacing();
			ImGui::TextWrapped("Drop PNG captures into the project's reference/ folder; they are listed here beside the live editor view. PCSX2 frame dumps and sheet scans both work.");
			ImGui::TextDisabled("Looking in: %s", referenceDir.string().c_str());
			return;
		}

		ImGui::TextDisabled("%d image%s in reference/", static_cast<int>(m_entries.size()), m_entries.size() == 1 ? "" : "s");
		ImGui::Separator();

		ImGui::BeginChild("##reference-list", ImVec2(240.0f, 0.0f), ImGuiChildFlags_ResizeX);
		for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
		{
			const Entry& entry = m_entries[static_cast<std::size_t>(i)];
			ImGui::PushID(i);
			if (ImGui::Selectable(entry.name.c_str(), i == m_selected))
			{
				m_selected = i;
				m_zoom = 1.0f;
				m_fit = true;
			}
			if (ImGui::IsItemHovered())
			{
				ImGui::SetTooltip("%s", entry.path.string().c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::BeginChild("##reference-view", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
		if (m_selected < 0)
		{
			ImGui::TextDisabled("Select an image on the left.");
			ImGui::EndChild();
			return;
		}

		const Entry& entry = m_entries[static_cast<std::size_t>(m_selected)];
		if (!LoadPreview(context, m_selected))
		{
			ImGui::PushStyleColor(ImGuiCol_Text, chrome::kError);
			ImGui::TextWrapped("%s", m_error.c_str());
			ImGui::PopStyleColor();
			ImGui::EndChild();
			return;
		}

		ImGui::Checkbox("Fit", &m_fit);
		ImGui::SameLine();
		ImGui::BeginDisabled(m_fit);
		ImGui::SetNextItemWidth(180.0f);
		ImGui::SliderFloat("##reference-zoom", &m_zoom, 0.05f, 8.0f, "zoom %.2fx", ImGuiSliderFlags_Logarithmic);
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("%s - %d x %d", entry.name.c_str(), m_previewWidth, m_previewHeight);

		if (m_previewImguiId == 0 || m_previewWidth <= 0 || m_previewHeight <= 0)
		{
			ImGui::EndChild();
			return;
		}

		const ImVec2 avail = ImGui::GetContentRegionAvail();
		float drawW = static_cast<float>(m_previewWidth);
		float drawH = static_cast<float>(m_previewHeight);
		if (m_fit)
		{
			const float scale = std::min(avail.x / drawW, avail.y / drawH);
			// Never upscale past native in fit mode: a 64x64 capture blown up over the pane
			// reads as a blur, and the zoom slider is right there for deliberate scaling.
			const float fit = std::min(scale, 1.0f);
			drawW *= fit;
			drawH *= fit;
		}
		else
		{
			drawW *= m_zoom;
			drawH *= m_zoom;
		}
		const ImVec2 size{std::max(drawW, 1.0f), std::max(drawH, 1.0f)};
		ImGui::Image(static_cast<ImTextureID>(m_previewImguiId), size);
		if (ImGui::IsItemHovered() && !m_fit)
		{
			ImGuiIO& io = ImGui::GetIO();
			if (io.MouseWheel != 0.0f)
			{
				m_zoom = std::clamp(m_zoom * (io.MouseWheel > 0.0f ? 1.15f : 1.0f / 1.15f), 0.05f, 8.0f);
			}
		}
		ImGui::EndChild();
	}
} // namespace aether::editor::twinsanity
