#pragma once

#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "KenneyImport.hpp"
#include "debug/DebugPanel.hpp"

namespace aether::editor
{
	// tools/assetpack/main.cpp's own CLI frontend aliases the same way.
	namespace kenney = aether::assetpipeline::kenney;

	// Human front end over the Kenney asset importer core
	// (tools/assetpack/KenneyImport.hpp). Browses the checked-in pack manifest, lists a
	// pack's models straight out of its cached zip, and runs the same ImportModel the
	// AssetPacker "kenney import" CLI subcommand calls - in process, so there is no
	// JSON-over-stdout boundary to cross here. See KenneyImport.hpp's header comment: this
	// panel and the CLI are the two front ends over one core, and neither reimplements
	// fetch/cache/extract/bake/collider-fit/credit/catalogue.
	class KenneyBrowserPanel final : public DebugPanel
	{
	public:
		std::string_view GetName() const override
		{
			return "Kenney Browser";
		}

		[[nodiscard]] bool DefaultVisible() const override
		{
			return false;
		}

		void OnDetach(app::LayerContext& context) override;
		void OnUpdate(app::LayerContext& context) override;
		void OnImGui(app::LayerContext& context) override;

	private:
		// Result of fetching + listing one pack's models, off the render thread: curl (cold
		// cache) plus a `tar -tf` listing - a cache hit is instant but is still routed
		// through the same worker so the caller never has to know which up front.
		struct PackModelsResult
		{
			bool ok = false;
			std::string error;
			bool wasAlreadyCached = false;
			std::vector<kenney::PackEntry> models;
		};

		// The pack-models fetch in flight. Held as a future rather than a raw thread so that
		// tearing the panel down joins it: a curl/tar subprocess still writing into
		// .temp/kenney-cache after the editor has torn down would be a crash on exit (same
		// reasoning as MaterialGraphPanel::PendingCompile over a slangc run).
		struct PendingPackLoad
		{
			std::future<PackModelsResult> future;
			std::string slug; // which pack this was fetching, for the m_packModels key
		};

		// The ImportModel run in flight. Same reasoning as PendingPackLoad: baking can take a
		// moment and must not stall the UI thread or survive the editor tearing down.
		struct PendingImport
		{
			std::future<kenney::ImportResult> future;
		};

		void EnsureManifestLoaded();
		void SelectPack(const std::string& slug);
		void SelectModel(const kenney::PackEntry& entry);
		void StartPackLoadIfNeeded(const kenney::PackInfo& pack);
		void StartImport(const kenney::PackInfo& pack, const std::filesystem::path& projectRoot);
		void PollPackLoad(app::LayerContext& context);
		void PollImport(app::LayerContext& context);
		void DrawPackList();
		void DrawPackDetail(app::LayerContext& context, const kenney::PackInfo& pack);
		void DrawModelList(const PackModelsResult& loaded);
		void DrawImportForm(app::LayerContext& context, const kenney::PackInfo& pack);
		void DrawImportResult() const;

		std::vector<kenney::PackInfo> m_packs;
		std::string m_manifestError;
		bool m_manifestLoaded = false;

		std::string m_selectedSlug;
		std::unordered_map<std::string, PackModelsResult> m_packModels;
		std::optional<PendingPackLoad> m_packLoad;

		char m_modelFilter[128] = {};
		std::string m_selectedZipMemberPath;
		std::string m_selectedFileName;

		char m_categoryBuf[128] = "Props";
		char m_propNameBuf[128] = {};
		char m_displayNameBuf[256] = {};
		float m_mass = 1.0f;
		bool m_registerInCatalog = true;

		std::optional<PendingImport> m_importing;
		bool m_hasImportResult = false;
		kenney::ImportResult m_importResult;
	};
} // namespace aether::editor
