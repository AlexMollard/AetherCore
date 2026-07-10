// AssetPacker - build-time tool that bundles a source-asset directory into a
// single binary (.pak) file consumed by the runtime PakBackend.
//
// Usage:
//   AssetPacker [--project] [--import-materials] [--compress-level N] <source-dir> <output.pak>
//   AssetPacker import-materials <source-dir>
//   AssetPacker bake-font <ttf> <outDir>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "AssetPipeline.hpp"
#include "FontProcessor.hpp"
#include "MaterialImporter.hpp"

using namespace aether::assetpipeline;

namespace fs = std::filesystem;

struct Args
{
	bool project = false;
	bool importMaterials = false;
	int compressionLevel = 3;
	fs::path sourceDir;
	fs::path outputPath;
};

static std::optional<Args> ParseArgs(int argc, char* argv[])
{
	Args args;
	int argOffset = 1;

	while (argOffset < argc)
	{
		const std::string arg = argv[argOffset];

		if (arg == "--import-materials")
		{
			args.importMaterials = true;
			++argOffset;
		}
		else if (arg == "--project" || arg == "pack-project")
		{
			args.project = true;
			++argOffset;
		}
		else if (arg == "--compress-level")
		{
			if (argOffset + 1 >= argc)
			{
				std::cerr << "AssetPacker: --compress-level requires a value (0-22)\n";
				return std::nullopt;
			}
			const std::string val = argv[argOffset + 1];
			auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), args.compressionLevel);
			if (ec != std::errc{})
			{
				std::cerr << "AssetPacker: invalid compression level '" << val << "'\n";
				return std::nullopt;
			}
			argOffset += 2;
		}
		else if (arg == "import-materials")
		{
			if (argc < argOffset + 2)
			{
				std::cerr << "Usage: AssetPacker import-materials <source-dir>\n";
				return std::nullopt;
			}
			const int result = MaterialImporter::ImportDirectory(fs::path(argv[argOffset + 1]));
			std::exit((result < 0) ? 1 : 0);
		}
		else if (arg == "bake-font")
		{
			if (argc < argOffset + 3)
			{
				std::cerr << "Usage: AssetPacker bake-font <ttf> <outDir>\n";
				return std::nullopt;
			}
			const fs::path ttfPath(argv[argOffset + 1]);
			const fs::path outDir(argv[argOffset + 2]);
			const FontProcessor::BakeResult result = FontProcessor::BakeFont(ttfPath, outDir);
			if (!result.success)
			{
				std::cerr << "AssetPacker: bake-font failed: " << result.error << "\n";
				std::exit(1);
			}
			std::cout << "AssetPacker: baked '" << ttfPath.generic_string() << "' -> " << result.glyphCount << " glyphs, atlas " << result.atlasWidth << "x" << result.atlasHeight << "\n";
			std::exit(0);
		}
		else
		{
			break;
		}
	}

	if (argc < argOffset + 2)
	{
		std::cerr << "Usage: AssetPacker [--project] [--import-materials] [--compress-level N] <source-dir> <output.pak>\n";
		std::cerr << "       AssetPacker import-materials <source-dir>\n";
		std::cerr << "       AssetPacker bake-font <ttf> <outDir>\n";
		return std::nullopt;
	}

	args.sourceDir = fs::path(argv[argOffset]);
	args.outputPath = fs::path(argv[argOffset + 1]);

	return args;
}

int main(int argc, char* argv[])
{
	const auto args = ParseArgs(argc, argv);
	if (!args)
	{
		return 1;
	}

	const PackOptions options{
	        .compressionLevel = args->compressionLevel,
	        .importMaterials = args->importMaterials,
	        .projectLayout = args->project,
	};
	const PackResult result = args->project
	        ? PackProject(args->sourceDir, args->outputPath, options)
	        : PackDirectory(args->sourceDir, args->outputPath, options);
	if (!result.ok)
	{
		std::cerr << "AssetPacker: " << result.message << "\n";
		return 1;
	}
	return 0;
}
