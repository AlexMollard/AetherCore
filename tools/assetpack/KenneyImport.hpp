#pragma once
#include <cstdint>
#include <filesystem>
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

	enum class ColliderShape
	{
		Auto,   // ImportModel's own safe default: an enclosing box from the raw AABB.
		Box,
		Sphere,
		Capsule,
		Cylinder,
		None, // Not a spawnable physics prop (e.g. a first-person viewmodel) - skip PropSpawner.cs registration entirely.
	};

	[[nodiscard]] std::string ToString(ColliderShape shape);
	[[nodiscard]] std::optional<ColliderShape> ParseColliderShape(const std::string& text);

	struct ColliderFit
	{
		ColliderShape shape = ColliderShape::Box;
		Vec3 halfExtents{0.0f, 0.0f, 0.0f}; // Box only
		float radius = 0.0f;                // Sphere/Capsule/Cylinder
		float halfHeight = 0.0f;             // Capsule/Cylinder
		Vec3 center{0.0f, 0.0f, 0.0f};        // entity-local offset, every shape
		Vec3 nativeSize{0.0f, 0.0f, 0.0f};     // full W x H x D of the raw AABB, for reporting
		std::vector<std::string> notes;       // caveats: square footprint, name suggests a taper, ...
	};

	// Derives a collider from the model's OWN glTF accessor bounds - composing each mesh
	// node's translation and scale (rotation is intentionally ignored, matching the manual
	// derivation this automates - see PropCatalogExtension.md's TrashCan note on why that is
	// an acceptable approximation for axis-aligned Kenney assets, not an oversight here).
	// `requestedShape` other than Auto/Box always succeeds (the caller has judged the model's
	// real shape, typically by eye against the pack preview image); Auto always resolves to a
	// safe enclosing Box - never a shape smaller than the mesh - and instead surfaces
	// uncertainty as `notes` (e.g. a near-square footprint that might actually be round, or a
	// filename suggesting a taper no primitive fits - see PropCatalogExtension.md's
	// convex-hull worklist) rather than silently guessing a rounder shape that could be wrong.
	[[nodiscard]] std::optional<ColliderFit> FitCollider(const std::filesystem::path& glbPath, const std::string& modelNameForHints, ColliderShape requestedShape, std::string& error);

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
		ColliderShape requestedShape = ColliderShape::Auto;
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

		ColliderFit collider;

		std::string creditsLine;
		bool creditsAppended = false;
		bool creditsAlreadyPresent = false;

		std::string catalogEntry;
		bool catalogAppended = false;
		bool catalogAlreadyPresent = false;
		bool catalogSkippedNoCollider = false; // requestedShape == None

		std::vector<std::string> warnings;
	};

	// The one real operation: ensure the pack is cached -> extract the requested model (plus
	// its material's shared texture, if any) into the project -> bake it (MeshProcessor,
	// same as the editor's drag-drop path) -> fit a collider from its own bounds -> append a
	// CREDITS.md provenance line -> append a PropSpawner.cs catalogue entry. Idempotent: an
	// identical second call reports what already exists instead of duplicating it, and never
	// overwrites a shared texture that already exists with DIFFERENT content (fails loudly
	// instead - see the doc comment on the write-tracking in the .cpp).
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
} // namespace aether::assetpipeline::kenney
