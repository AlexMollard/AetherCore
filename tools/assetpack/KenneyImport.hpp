#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "PipelineUtils.hpp"

// One core, two front ends: the AssetPacker "kenney" CLI subcommand (shelled out to by the
// AetherCore MCP kenney_* tools, for agents) and the Editor's Kenney Browser panel (linked
// in-process, for humans) both call ImportModel below and nothing else. Neither reimplements
// fetch/cache/extract/bake/collider-fit/credit/catalogue - see AGENTS.md's Kenney import
// section and projects/Sandbox/assets/PropCatalogExtension.md for the manual workflow this
// automates.
namespace aether::assetpipeline::kenney
{
	struct PackInfo
	{
		std::string slug;
		std::string name;
		std::string version;
		std::string pageUrl;
		std::string zipUrl;
		std::string previewImageUrl;
		std::string modelDir; // pack-relative folder holding importable .glb files, e.g. "Models/GLB format"
		std::string license;
		std::string licenseUrl;
		std::string author;
	};

	// Parses the checked-in TOML manifest (kenney_packs.toml, next to this header). Returns
	// an empty vector and fills `error` on a parse failure; a manifest with zero [[pack]]
	// entries is not an error (an empty, valid file).
	[[nodiscard]] std::vector<PackInfo> LoadManifest(const std::filesystem::path& manifestPath, std::string& error);
	[[nodiscard]] std::optional<PackInfo> FindPack(const std::vector<PackInfo>& packs, const std::string& slug);

	struct CacheResult
	{
		bool ok = false;
		std::filesystem::path zipPath;
		bool wasAlreadyCached = false;
		std::string error;
	};

	// Pack-level caching, not an optimisation: downloads (via a `curl` subprocess - no HTTP
	// client is linked into this dependency-light tool tree) into
	// `cacheDir/<slug>-<version>.zip` only if that file is not already present and non-empty.
	// Every model out of the same pack after the first reuses this file with zero network
	// calls - this is what makes "eight models, one download" true.
	[[nodiscard]] CacheResult EnsurePackCached(const PackInfo& pack, const std::filesystem::path& cacheDir);

	struct PackEntry
	{
		std::string zipMemberPath; // full path inside the zip, e.g. "Models/GLB format/box-small.glb"
		std::string fileName;      // "box-small.glb"
		// No size here: a robust cross-platform `tar -tv` size parse (format differs between
		// bsdtar and GNU tar, and names may contain spaces) isn't worth it for a listing
		// field nothing depends on. ImportResult reports the real on-disk size of whatever
		// was actually imported.
	};

	// Lists every file matching one of `extensions` (lowercase, with the dot, e.g.
	// {".glb", ".gltf"} or {".png"}) directly under `subDir` inside the cached zip - via a
	// `tar -tf` subprocess (bsdtar/libarchive reads zip natively on Windows 10+ and most
	// Linux distros; see EnsurePackCached's own comment on why a subprocess at all).
	// Excludes nested subfolders (e.g. a shared Textures/ next to the models) and anything
	// outside `subDir`. `extensions` defaults to the model formats this importer targets;
	// pass e.g. {".png"} to list a texture-only pack instead - same pack cache, same zip
	// listing, just a different filter, since a Kenney pack's directory layout doesn't care
	// what ends up consuming its contents.
	[[nodiscard]] std::vector<PackEntry> ListPackModels(const std::filesystem::path& zipPath, const std::string& subDir, std::string& error, const std::vector<std::string>& extensions = {".glb", ".gltf"});

	struct ImportRequest
	{
		PackInfo pack;
		std::string zipMemberPath; // which file inside the pack to import, from ListPackModels
		std::filesystem::path projectRoot;
		std::filesystem::path cacheDir;
		std::string category;  // destination folder under assets/models/, e.g. "Props"
		std::string propName;  // PascalCase file stem, e.g. "MachineFortified"
		std::string displayName; // catalogue display name, e.g. "Machine Fortified"
		float mass = 1.0f;
		// Colliders are NOT chosen here: the catalogue moved to convex hulls
		// (Physics.AddConvexHullBody builds the hull from each model's own baked vertices
		// at spawn time), so a catalogue entry carries no collider geometry at all - which
		// is also why there is no per-import shape to pick. This flag only decides
		// whether the model becomes a spawnable catalogue entry at all (false for
		// viewmodel-style packs that must never appear in the spawn menu).
		bool registerInCatalog = true;
		// Relative to projectRoot; default matches every prior manual import in this project.
		std::string creditsRelPath = "assets/CREDITS.md";
		std::string catalogRelPath = "scripts/PropSpawner.cs";
	};

	struct ImportResult
	{
		bool ok = false;
		std::string error;

		std::filesystem::path modelPath;   // written .glb on disk
		bool modelAlreadyPresent = false;  // idempotent re-import: file already existed, untouched
		std::filesystem::path texturePath; // written shared texture on disk, if the material referenced one
		bool textureAlreadyPresent = false;
		bool bakedNow = false; // MeshProcessor ran and wrote .mesh/.material this call

		Vec3 nativeSize{0.0f, 0.0f, 0.0f}; // full W x H x D of the model's raw bounds, for display

		std::string creditsLine;
		bool creditsAppended = false;
		bool creditsAlreadyPresent = false;

		std::string catalogEntry;
		bool catalogAppended = false;
		bool catalogAlreadyPresent = false;
		bool catalogSkipped = false; // registerInCatalog == false

		std::vector<std::string> warnings;
	};

	// The one real operation: ensure the pack is cached -> extract the requested model (plus
	// its material's shared texture, if any) into the project -> bake it (MeshProcessor,
	// same as the editor's drag-drop path) -> append a CREDITS.md provenance line -> append
	// a PropSpawner.cs catalogue entry (a bare model reference - PropSpawner builds its
	// collider as a convex hull from the baked mesh itself, not from anything computed
	// here). Idempotent: an identical second call reports what already exists instead of
	// duplicating it, and never overwrites a shared texture that already exists with
	// DIFFERENT content (fails loudly instead - see the doc comment on the write-tracking
	// in the .cpp).
	[[nodiscard]] ImportResult ImportModel(const ImportRequest& request);

	// A genuinely different pipeline, not a parameter on ImportModel: some Kenney packs
	// (Input Prompts among them) are icon FONTS, not meshes - FontProcessor::BakeFont
	// (glyph-outline curves, see FontProcessor.hpp) is the correct baker, the same one
	// `AssetPacker bake-font` and every hand-authored project font in this repo already
	// go through. There is no collider, no PropSpawner.cs entry, no MeshProcessor
	// involved at all - a baked font is consumed by name via Ui.SetFont, nothing else.
	struct FontImportRequest
	{
		PackInfo pack;
		std::string zipMemberPath;     // the .ttf/.otf inside the zip
		std::string charMapZipMemberPath; // optional glyph-name -> codepoint reference text file; empty = skip
		std::filesystem::path projectRoot;
		std::filesystem::path cacheDir;
		std::string fontName; // output stem: assets/fonts/<fontName>.ttf/.fontcurves, and the exact
		                       // string a script passes to Ui.SetFont(entity, fontName)
		// Relative to projectRoot; matches ImportRequest's own default and every prior
		// manual import in this project.
		std::string creditsRelPath = "assets/CREDITS.md";
	};

	struct FontImportResult
	{
		bool ok = false;
		std::string error;

		std::filesystem::path fontPath;    // written .ttf/.otf on disk (source, committed - matches
		                                    // this project's existing font convention, unlike models'
		                                    // gitignored .mesh)
		bool fontAlreadyPresent = false;
		std::filesystem::path charMapPath; // written glyph-name reference text, if requested; empty if not
		std::filesystem::path curvesPath;  // baked .fontcurves on disk (also committed, same convention)
		bool bakedNow = false;
		std::uint32_t glyphCount = 0;

		std::string creditsLine;
		bool creditsAppended = false;
		bool creditsAlreadyPresent = false;

		std::vector<std::string> warnings;
	};

	// Ensures the pack is cached -> extracts the requested font (plus its glyph-map
	// reference text, if given) into assets/fonts/ -> bakes it (FontProcessor::BakeFont,
	// the same function `AssetPacker bake-font` calls - not a subprocess to that CLI, a
	// direct call to the same library function, exactly like the mesh path's relationship
	// to `AssetPacker bake`) -> appends a CREDITS.md provenance line. Idempotent like
	// ImportModel: a second call with the same fontName reports what already exists.
	[[nodiscard]] FontImportResult ImportFont(const FontImportRequest& request);

	// Deterministic naming for bulk imports, shared by every front end (CLI, MCP, panel)
	// so they cannot drift apart. These are SUGGESTIONS - a single-model import may
	// overtype them; a bulk run takes them as-is, suffixing on the rare stem collision.
	//
	//   SuggestPropName("machine-fortified.glb")  -> "MachineFortified"
	//   SuggestDisplayName("MachineFortified")    -> "Machine Fortified"
	//   SuggestCategory("factory-kit")            -> "FactoryKit"
	[[nodiscard]] std::string SuggestPropName(const std::string& fileName);
	[[nodiscard]] std::string SuggestDisplayName(const std::string& propName);
	[[nodiscard]] std::string SuggestCategory(const std::string& packSlug);

	// Thread-safe progress a UI can poll while an ImportModels run is in flight on a
	// worker. The worker owns writes (Update); Read() is what a render-thread caller
	// calls once per frame.
	struct BulkProgress
	{
		struct State
		{
			int done = 0;
			int total = 0;
			std::string currentFile;
		};
		void Update(int done, int total, std::string currentFile);
		[[nodiscard]] State Read() const;

	private:
		mutable std::mutex m_mutex;
		State m_state;
	};

	struct BulkImportRequest
	{
		PackInfo pack;
		std::filesystem::path projectRoot;
		std::filesystem::path cacheDir;
		// One destination folder under assets/models/ for the whole run. Empty means
		// SuggestCategory(pack.slug) - one category per pack, the deliberate default (a
		// pack's models share one material atlas and one visual identity).
		std::string category;
		// Case-insensitive substring match on the zip member's file name; empty = every
		// model in the pack. "Import the whole pack" is just the empty filter - the
		// honest primitive is always "import the current selection set".
		std::string filter;
		float mass = 1.0f;
		bool registerInCatalog = true;
		// Optional hooks for a UI driving this off the render thread. `progress` is
		// updated after every item; `cancel` is polled BETWEEN items only - never
		// mid-item, because each item is atomic (tracked-write with rollback), and a
		// cancel honoured mid-item would be the one way a bulk run could leave a
		// partial file behind.
		BulkProgress* progress = nullptr;
		const std::atomic_bool* cancel = nullptr;
	};

	struct BulkImportResult
	{
		bool ok = false; // false only when the pack itself could not be prepared/listed
		bool cancelled = false;
		std::string error;

		int matched = 0;        // models matching the filter
		int imported = 0;       // newly landed this run
		int alreadyPresent = 0; // model file already existed; skipped, nothing duplicated
		int failed = 0;
		// One bad model never poisons the batch: every failure is named with its reason
		// here, and the rest of the run continues.
		std::vector<std::string> failures; // "<fileName>: <reason>"
		std::vector<std::string> warnings; // de-duplicated across items
		std::vector<std::filesystem::path> modelPaths;
	};

	// Bulk front of the same per-item ImportModel path (not a reimplementation): caches
	// the pack ONCE, then runs ImportModel per matching entry with deterministic
	// SuggestPropName/SuggestDisplayName naming (suffixing on the rare stem collision).
	// Safe to re-run: per-item idempotency composes, so a second run of the same filter
	// reports everything as alreadyPresent and duplicates nothing.
	[[nodiscard]] BulkImportResult ImportModels(const BulkImportRequest& request);
} // namespace aether::assetpipeline::kenney
