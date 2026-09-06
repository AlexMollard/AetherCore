#include "debug/KenneyBrowserPanel.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>

#include <imgui.h>

#include "AetherCore.hpp"
#include "Color.hpp"
#include "debug/DebugPanel.hpp"
#include "debug/EditorChrome.hpp"
#include "debug/Icons.hpp"
#include "editor/EditorProjectContext.hpp"
#include "layers/AppLayer.hpp"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>

#	include <shellapi.h>
#endif

namespace aether::editor
{
	namespace
	{
		// The combo/lookup order both the Collider Shape combo and StartImport share, so an
		// index picked in one is always the shape sent in the other. Labels match
		// kenney::ToString(shape) exactly (lowercase - the same text the CLI's
		// ParseColliderShape accepts), so nothing here can drift out of sync with the core.
		constexpr std::array<kenney::ColliderShape, 6> kColliderShapeOrder = {
		        kenney::ColliderShape::Auto,
		        kenney::ColliderShape::Box,
		        kenney::ColliderShape::Sphere,
		        kenney::ColliderShape::Capsule,
		        kenney::ColliderShape::Cylinder,
		        kenney::ColliderShape::None,
		};

		const char* const kColliderShapeLabels[] = {"auto", "box", "sphere", "capsule", "cylinder", "none"};

		ImVec4 ToImVec4(const glm::vec4& c) noexcept
		{
			return {c.r, c.g, c.b, c.a};
		}

		void OpenUrlInOS(const std::string& url)
		{
			if (url.empty())
			{
				return;
			}
#ifdef _WIN32
			ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
			std::system(("xdg-open \"" + url + "\" &").c_str());
#endif
		}

		bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
		{
			if (needle.empty())
			{
				return true;
			}
			const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
			return it != haystack.end();
		}

		template<std::size_t N>
		void CopyToBuffer(char (&buffer)[N], std::string_view text)
		{
			const std::size_t len = std::min(text.size(), N - 1);
			std::memcpy(buffer, text.data(), len);
			buffer[len] = '\0';
		}

		// "machine-fortified.glb" -> "MachineFortified": strip the model extension, then
		// PascalCase every hyphen/underscore/space-separated word. A human-facing default the
		// user can overtype in the form, not a hard naming rule.
		std::string SuggestPropName(std::string_view fileName)
		{
			std::string stem(fileName);
			for (std::string_view ext: {".glb", ".gltf"})
			{
				if (stem.size() > ext.size() && ContainsCaseInsensitive(std::string_view(stem).substr(stem.size() - ext.size()), ext))
				{
					stem.resize(stem.size() - ext.size());
					break;
				}
			}
			std::string out;
			out.reserve(stem.size());
			bool capitalizeNext = true;
			for (const char c: stem)
			{
				if (c == '-' || c == '_' || c == ' ')
				{
					capitalizeNext = true;
					continue;
				}
				out += capitalizeNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
				capitalizeNext = false;
			}
			return out;
		}

		// "MachineFortified" -> "Machine Fortified": a space before every internal capital.
		std::string SuggestDisplayName(std::string_view propName)
		{
			std::string out;
			out.reserve(propName.size() + 4);
			for (std::size_t i = 0; i < propName.size(); ++i)
			{
				const char c = propName[i];
				if (i > 0 && std::isupper(static_cast<unsigned char>(c)) != 0)
				{
					out += ' ';
				}
				out += c;
			}
			return out;
		}

		void DrawMetricRowFormatted(const char* label, const std::string& value)
		{
			DrawMetricRow(label, value.c_str());
		}
	} // namespace

	void KenneyBrowserPanel::OnDetach(app::LayerContext& /*context*/)
	{
		// Both futures are std::async and block in their destructor until the worker
		// finishes; resetting them here explicitly (rather than only relying on the panel's
		// own destructor running later) guarantees no curl/tar/bake subprocess is still
		// writing into .temp/kenney-cache or a project's assets folder once the layer that
		// owns this panel starts tearing down.
		m_packLoad.reset();
		m_importing.reset();
	}

	void KenneyBrowserPanel::OnUpdate(app::LayerContext& context)
	{
		PollPackLoad(context);
		PollImport(context);
	}

	void KenneyBrowserPanel::EnsureManifestLoaded()
	{
		if (m_manifestLoaded)
		{
			return;
		}
		m_manifestLoaded = true;
		m_manifestError.clear();
		m_packs = kenney::LoadManifest(AETHERCORE_KENNEY_MANIFEST, m_manifestError);
	}

	void KenneyBrowserPanel::SelectPack(const std::string& slug)
	{
		if (m_selectedSlug == slug)
		{
			return;
		}
		m_selectedSlug = slug;
		m_selectedZipMemberPath.clear();
		m_selectedFileName.clear();
		m_modelFilter[0] = '\0';
	}

	void KenneyBrowserPanel::SelectModel(const kenney::PackEntry& entry)
	{
		if (m_selectedZipMemberPath == entry.zipMemberPath)
		{
			return;
		}
		m_selectedZipMemberPath = entry.zipMemberPath;
		m_selectedFileName = entry.fileName;
		const std::string suggestedProp = SuggestPropName(entry.fileName);
		CopyToBuffer(m_propNameBuf, suggestedProp);
		CopyToBuffer(m_displayNameBuf, SuggestDisplayName(suggestedProp));
	}

	void KenneyBrowserPanel::StartPackLoadIfNeeded(const kenney::PackInfo& pack)
	{
		if (m_packModels.contains(pack.slug))
		{
			return;
		}
		if (m_packLoad)
		{
			// One fetch at a time - the same reasoning as MaterialGraphPanel's RequestCompile.
			// This pack's turn comes as soon as the in-flight one lands and this function
			// runs again next frame.
			return;
		}

		PendingPackLoad pending;
		pending.slug = pack.slug;
		const kenney::PackInfo packCopy = pack;
		pending.future = std::async(std::launch::async,
		        [packCopy]() -> PackModelsResult
		        {
			        PackModelsResult out;
			        const std::filesystem::path cacheDir(AETHERCORE_KENNEY_CACHE_DIR);
			        const kenney::CacheResult cache = kenney::EnsurePackCached(packCopy, cacheDir);
			        if (!cache.ok)
			        {
				        out.error = cache.error;
				        return out;
			        }
			        out.wasAlreadyCached = cache.wasAlreadyCached;
			        std::string listError;
			        out.models = kenney::ListPackModels(cache.zipPath, packCopy.modelDir, listError);
			        if (!listError.empty())
			        {
				        out.error = listError;
				        return out;
			        }
			        out.ok = true;
			        return out;
		        });
		m_packLoad = std::move(pending);
	}

	void KenneyBrowserPanel::StartImport(const kenney::PackInfo& pack, const std::filesystem::path& projectRoot)
	{
		if (m_importing)
		{
			return;
		}

		kenney::ImportRequest request;
		request.pack = pack;
		request.zipMemberPath = m_selectedZipMemberPath;
		request.projectRoot = projectRoot;
		request.cacheDir = std::filesystem::path(AETHERCORE_KENNEY_CACHE_DIR);
		request.category = m_categoryBuf;
		request.propName = m_propNameBuf;
		request.displayName = m_displayNameBuf;
		request.mass = m_mass;
		request.requestedShape = kColliderShapeOrder[static_cast<std::size_t>(m_colliderShapeIndex)];

		PendingImport pending;
		pending.future = std::async(std::launch::async, [request]() { return kenney::ImportModel(request); });
		m_importing = std::move(pending);
		m_hasImportResult = false;
	}

	void KenneyBrowserPanel::PollPackLoad(app::LayerContext& context)
	{
		if (!m_packLoad)
		{
			return;
		}
		// Keeps the editor from throttling to its idle framerate while curl/tar are running
		// in the background, the same way MaterialGraphPanel::PollCompile does for slangc.
		if (auto* engine = context.TryGet<AetherCore>())
		{
			engine->RequestActivity();
		}
		if (m_packLoad->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			return;
		}

		PendingPackLoad finished = std::move(*m_packLoad);
		m_packLoad.reset();

		PackModelsResult result;
		try
		{
			result = finished.future.get();
		}
		catch (const std::exception& ex)
		{
			result.ok = false;
			result.error = std::string("Kenney pack fetch failed: ") + ex.what();
		}
		m_packModels[finished.slug] = std::move(result);
	}

	void KenneyBrowserPanel::PollImport(app::LayerContext& context)
	{
		if (!m_importing)
		{
			return;
		}
		if (auto* engine = context.TryGet<AetherCore>())
		{
			engine->RequestActivity();
		}
		if (m_importing->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			return;
		}

		PendingImport finished = std::move(*m_importing);
		m_importing.reset();

		try
		{
			m_importResult = finished.future.get();
		}
		catch (const std::exception& ex)
		{
			m_importResult = kenney::ImportResult{};
			m_importResult.ok = false;
			m_importResult.error = std::string("Kenney import failed: ") + ex.what();
		}
		m_hasImportResult = true;
	}

	void KenneyBrowserPanel::DrawPackList()
	{
		ImGui::SeparatorText("Packs");
		ImGui::BeginChild("##kenneyPackList", ImVec2(0.0f, 110.0f), ImGuiChildFlags_Borders);
		for (const kenney::PackInfo& pack: m_packs)
		{
			const bool selected = pack.slug == m_selectedSlug;
			const std::string label = std::format("{}  (v{}, {})", pack.name, pack.version, pack.license);
			if (ImGui::Selectable(label.c_str(), selected))
			{
				SelectPack(pack.slug);
			}
		}
		ImGui::EndChild();
	}

	void KenneyBrowserPanel::DrawModelList(const PackModelsResult& loaded)
	{
		ImGui::SeparatorText("Models");
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##kenneyModelFilter", ICON_FA_MAGNIFYING_GLASS "  Filter models", m_modelFilter, sizeof(m_modelFilter));

		std::vector<const kenney::PackEntry*> filtered;
		filtered.reserve(loaded.models.size());
		for (const kenney::PackEntry& entry: loaded.models)
		{
			if (m_modelFilter[0] != '\0' && !ContainsCaseInsensitive(entry.fileName, m_modelFilter))
			{
				continue;
			}
			filtered.push_back(&entry);
		}

		ImGui::BeginChild("##kenneyModelList", ImVec2(0.0f, 150.0f), ImGuiChildFlags_Borders);
		for (const kenney::PackEntry* entry: filtered)
		{
			const bool selected = entry->zipMemberPath == m_selectedZipMemberPath;
			if (ImGui::Selectable(entry->fileName.c_str(), selected))
			{
				SelectModel(*entry);
			}
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%zu / %zu models", filtered.size(), loaded.models.size());
	}

	void KenneyBrowserPanel::DrawImportForm(app::LayerContext& context, const kenney::PackInfo& pack)
	{
		ImGui::SeparatorText("Import");
		ImGui::Text("Model: %s", m_selectedFileName.c_str());

		ImGui::InputText("Category", m_categoryBuf, sizeof(m_categoryBuf));
		ImGui::InputText("Prop Name", m_propNameBuf, sizeof(m_propNameBuf));
		ImGui::InputText("Display Name", m_displayNameBuf, sizeof(m_displayNameBuf));
		ImGui::InputFloat("Mass", &m_mass);
		ImGui::Combo("Collider Shape", &m_colliderShapeIndex, kColliderShapeLabels, static_cast<int>(std::size(kColliderShapeLabels)));

		const auto* project = context.TryGet<app::EditorProjectContext>();
		const bool hasProject = project != nullptr && project->IsLoaded();
		const bool fieldsFilled = m_categoryBuf[0] != '\0' && m_propNameBuf[0] != '\0' && m_displayNameBuf[0] != '\0';
		const bool importing = m_importing.has_value();

		ImGui::BeginDisabled(!hasProject || !fieldsFilled || importing);
		if (chrome::PrimaryButton(importing ? "Importing..." : ICON_FA_DOWNLOAD "  Import"))
		{
			StartImport(pack, project->root);
		}
		ImGui::EndDisabled();
		if (!hasProject)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("Open a project to import.");
		}
	}

	void KenneyBrowserPanel::DrawImportResult() const
	{
		if (!m_hasImportResult)
		{
			return;
		}

		ImGui::SeparatorText("Import Result");
		if (!m_importResult.ok)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ToImVec4(colors::Error), "%s", m_importResult.error.c_str());
			ImGui::PopTextWrapPos();
			return;
		}

		ImGui::TextColored(ToImVec4(colors::Success), "Import succeeded");
		if (ImGui::BeginTable("KenneyImportResult", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawMetricRow("Model", m_importResult.modelPath.generic_string().c_str());
			DrawMetricRow("Model status", m_importResult.modelAlreadyPresent ? "already present" : "written");
			if (!m_importResult.texturePath.empty())
			{
				DrawMetricRow("Texture", m_importResult.texturePath.generic_string().c_str());
				DrawMetricRow("Texture status", m_importResult.textureAlreadyPresent ? "already present" : "written");
			}
			DrawMetricRow("Baked now", m_importResult.bakedNow ? "yes" : "no");

			const kenney::ColliderFit& collider = m_importResult.collider;
			DrawMetricRow("Collider shape", kenney::ToString(collider.shape).c_str());
			switch (collider.shape)
			{
				case kenney::ColliderShape::Sphere:
					DrawMetricRowFormatted("Collider radius", std::format("{:.3f}", collider.radius));
					break;
				case kenney::ColliderShape::Capsule:
				case kenney::ColliderShape::Cylinder:
					DrawMetricRowFormatted("Collider radius", std::format("{:.3f}", collider.radius));
					DrawMetricRowFormatted("Collider half height", std::format("{:.3f}", collider.halfHeight));
					break;
				case kenney::ColliderShape::None:
					break;
				case kenney::ColliderShape::Box:
				case kenney::ColliderShape::Auto:
				default:
					DrawMetricRowFormatted("Collider half extents", std::format("{:.3f}, {:.3f}, {:.3f}", collider.halfExtents.x, collider.halfExtents.y, collider.halfExtents.z));
					break;
			}
			DrawMetricRowFormatted("Collider center", std::format("{:.3f}, {:.3f}, {:.3f}", collider.center.x, collider.center.y, collider.center.z));
			DrawMetricRowFormatted("Native size", std::format("{:.3f} x {:.3f} x {:.3f}", collider.nativeSize.x, collider.nativeSize.y, collider.nativeSize.z));

			DrawMetricRow("Credits", m_importResult.creditsAlreadyPresent ? "already present" : (m_importResult.creditsAppended ? "appended" : "not written"));
			DrawMetricRow("Catalog", m_importResult.catalogSkippedNoCollider ? "skipped (no collider)" : (m_importResult.catalogAlreadyPresent ? "already present" : (m_importResult.catalogAppended ? "appended" : "not written")));
			ImGui::EndTable();
		}

		for (const std::string& note: m_importResult.collider.notes)
		{
			ImGui::TextColored(ToImVec4(colors::Warn), ICON_FA_TRIANGLE_EXCLAMATION "  %s", note.c_str());
		}
		for (const std::string& warning: m_importResult.warnings)
		{
			ImGui::TextColored(ToImVec4(colors::Warn), ICON_FA_TRIANGLE_EXCLAMATION "  %s", warning.c_str());
		}
	}

	void KenneyBrowserPanel::DrawPackDetail(app::LayerContext& context, const kenney::PackInfo& pack)
	{
		ImGui::SeparatorText(pack.name.c_str());
		if (ImGui::BeginTable("KenneyPackInfo", 2, ImGuiTableFlags_SizingStretchProp))
		{
			DrawMetricRow("Version", pack.version.c_str());
			DrawMetricRow("License", pack.license.c_str());
			DrawMetricRow("Author", pack.author.c_str());
			ImGui::EndTable();
		}
		ImGui::BeginDisabled(pack.pageUrl.empty());
		if (chrome::OutlineButton(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE "  Open Pack Page"))
		{
			OpenUrlInOS(pack.pageUrl);
		}
		ImGui::EndDisabled();

		StartPackLoadIfNeeded(pack);

		const bool loadingThisPack = m_packLoad && m_packLoad->slug == pack.slug;
		const auto it = m_packModels.find(pack.slug);
		if (loadingThisPack)
		{
			ImGui::TextDisabled("Fetching pack (download + list models)...");
			return;
		}
		if (it == m_packModels.end())
		{
			ImGui::TextDisabled("Waiting for the current pack fetch to finish...");
			return;
		}
		if (!it->second.ok)
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ToImVec4(colors::Error), "%s", it->second.error.c_str());
			ImGui::PopTextWrapPos();
			return;
		}

		DrawModelList(it->second);

		if (!m_selectedZipMemberPath.empty())
		{
			DrawImportForm(context, pack);
		}
	}

	void KenneyBrowserPanel::OnImGui(app::LayerContext& context)
	{
		ImGui::Begin("Kenney Browser", VisiblePtr());
		chrome::PanelHeader("KENNEY BROWSER");

		EnsureManifestLoaded();

		if (!m_manifestError.empty())
		{
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextColored(ToImVec4(colors::Error), "%s", m_manifestError.c_str());
			ImGui::PopTextWrapPos();
		}

		if (m_packs.empty())
		{
			ImGui::TextDisabled("No Kenney packs in the manifest.");
			ImGui::End();
			return;
		}

		DrawPackList();

		const auto packIt = std::ranges::find_if(m_packs, [this](const kenney::PackInfo& pack) { return pack.slug == m_selectedSlug; });
		if (packIt != m_packs.end())
		{
			DrawPackDetail(context, *packIt);
		}
		else
		{
			ImGui::TextDisabled("Select a pack above.");
		}

		DrawImportResult();

		ImGui::End();
	}
} // namespace aether::editor
