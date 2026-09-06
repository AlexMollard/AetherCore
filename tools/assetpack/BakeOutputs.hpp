#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "MeshProcessor.hpp"

// The one place a MeshProcessor::ProcessedResult gets written to disk from a
// dependency-light (no Engine/VFS) caller. `src/app/editor/ModelBake.cpp`'s
// EnsureModelBaked has its own copy of this for the in-process Editor case (it needs
// io::FileSystem/EditorProjectContext, unavailable here) - everything OUTSIDE the Editor
// process that bakes a model (the `AssetPacker bake` CLI subcommand, the Kenney importer)
// goes through this instead of each rolling its own writer.
namespace aether::assetpipeline
{
	struct BakeWriteResult
	{
		bool ok = false;
		std::string error;
		std::filesystem::path meshPath;
		std::vector<std::string> warnings; // e.g. a shared material already exists with different content
	};

	// Writes `baked`'s outputs next to `modelDiskPath` (<stem>.mesh/.skel/.animset), animation
	// clips under `projectRoot/animations`, and materials at their own MeshProcessor-computed
	// project-relative paths. Tracks every file THIS call writes and removes all of them if a
	// later write fails, so a partial bake never leaves a stale ".mesh exists" signal for a
	// caller that gates re-baking on that file's presence. A material that already exists with
	// DIFFERENT content is still overwritten (Kenney-style kits deliberately share one material
	// by name across many models) but reported in `warnings`, never silently swallowed.
	[[nodiscard]] BakeWriteResult WriteBakedOutputs(const MeshProcessor::ProcessedResult& baked, const std::filesystem::path& modelDiskPath, const std::filesystem::path& projectRoot);
} // namespace aether::assetpipeline
