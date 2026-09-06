#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Walks every `.glb`/`.gltf` under `<projectRoot>/assets/models/` and (re)bakes whichever
// is missing its `.mesh` output or is newer than it - source freshness includes external
// buffer/image dependencies (MeshProcessor::CollectExternalSourceFiles), so a `.gltf`'s
// EXTERNAL `.bin`/texture edit is exactly as stale-triggering as editing the `.gltf` itself.
//
// The one shared definition of "what needs baking", used by both the `AssetPacker bake-all`
// CLI subcommand (main.cpp) and `PublishSteps.cpp`'s publish-time bake step. Those two used
// to disagree: the publish step scanned scene/prefab `.toml` files for `project://...glb`
// substrings, which is blind to anything referenced only from a script (e.g.
// `PropSpawner.cs`'s spawn catalogue array) - exactly how models shipped with no baked mesh
// at all this session. A directory walk over model FILES has no such blind spot: a file
// that exists gets baked, however it is or isn't referenced.
namespace aether::assetpipeline
{
	struct BakeAllResult
	{
		int baked = 0;
		int upToDate = 0;
		int skipped = 0; // no mesh geometry produced (e.g. an animation-only glTF source) - not an error; see BakeAllModels' comment on why this is inherently ambiguous
		int failed = 0;  // read or write failure - a real error
		std::vector<std::string> bakedModels;   // project-relative paths of everything actually (re)baked
		std::vector<std::string> skippedModels; // "<project-relative path>: <reason>"
		std::vector<std::string> failures;      // "<project-relative path>: <reason>"
	};

	[[nodiscard]] BakeAllResult BakeAllModels(const std::filesystem::path& projectRoot);
} // namespace aether::assetpipeline
