// AssetPacker - build-time tool that bundles a source-asset directory into a
// single binary (.pak) file consumed by the runtime PakBackend.
//
// Usage:
//   AssetPacker [--import-materials] [--compress-level N] <source-dir> <output.pak>
//   AssetPacker import-materials <source-dir>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "MaterialImporter.hpp"
#include "PakWriter.hpp"

namespace fs = std::filesystem;

struct Args
{
	bool     importMaterials  = false;
	int      compressionLevel = 3;
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
		else
		{
			break;
		}
	}

	if (argc < argOffset + 2)
	{
		std::cerr << "Usage: AssetPacker [--import-materials] [--compress-level N] <source-dir> <output.pak>\n";
		std::cerr << "       AssetPacker import-materials <source-dir>\n";
		return std::nullopt;
	}

	args.sourceDir  = fs::path(argv[argOffset]);
	args.outputPath = fs::path(argv[argOffset + 1]);

	return args;
}

int main(int argc, char* argv[])
{
	const auto args = ParseArgs(argc, argv);
	if (!args)
		return 1;

	if (!fs::is_directory(args->sourceDir))
	{
		std::cerr << "AssetPacker: source directory not found: " << args->sourceDir << "\n";
		return 1;
	}

	if (args->importMaterials)
	{
		if (MaterialImporter::ImportDirectory(args->sourceDir) < 0)
			return 1;
	}

	PakWriter writer(args->compressionLevel);
	writer.AddDirectory(args->sourceDir);
	return writer.Write(args->outputPath) ? 0 : 1;
}
