#pragma once

// Public entry point for the asset pipeline. Callers (the editor, the CLI, tests)
// include ONLY this header — it exposes no third-party or internal packer types,
// so linking AssetPipeline into the engine stays clean.

#include <cstdint>
#include <filesystem>
#include <string>

namespace aether::assetpipeline
{
	struct PackOptions
	{
		int  compressionLevel = 3;
		bool importMaterials  = false; // run MaterialImporter before packing
		bool projectLayout    = false; // require ProjectSettings.toml; import from assets/ subdir
	};

	struct PackResult
	{
		bool                  ok = false;
		std::string           message;     // human-readable summary or failure reason
		std::uint64_t         sourceFiles = 0;
		std::uintmax_t        pakBytes = 0;
		std::filesystem::path outputPath;
	};

	// Pack a project root: optional material import + project descriptor check.
	[[nodiscard]] PackResult PackProject(const std::filesystem::path& projectRoot,
	                                      const std::filesystem::path& outputPak,
	                                      const PackOptions& options);

	// Pack an arbitrary asset directory (used for engine.pak).
	[[nodiscard]] PackResult PackDirectory(const std::filesystem::path& sourceDir,
	                                        const std::filesystem::path& outputPak,
	                                        const PackOptions& options);
}
