// AssetPacker - build-time tool that bundles a source-asset directory into a
// single binary (.pak) file consumed by the runtime PakBackend.
//
// Usage:
//   AssetPacker [--import-materials] [--compress-level N] <source-dir> <output.pak>
//   AssetPacker import-materials <source-dir>

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>

#include "MaterialImporter.hpp"
#include "PakWriter.hpp"

namespace fs = std::filesystem;

int main(int argc, char* argv[])
{
	bool importMaterials  = false;
	int  compressionLevel = 3; // zstd default; 0 = disabled
	int  argOffset        = 1;

	// Parse flags before positional arguments.
	while (argOffset < argc)
	{
		const std::string arg = argv[argOffset];

		if (arg == "--import-materials")
		{
			importMaterials = true;
			++argOffset;
		}
		else if (arg == "--compress-level")
		{
			if (argOffset + 1 >= argc)
			{
				std::cerr << "AssetPacker: --compress-level requires a value (0-22)\n";
				return 1;
			}
			const std::string val = argv[argOffset + 1];
			auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), compressionLevel);
			if (ec != std::errc{})
			{
				std::cerr << "AssetPacker: invalid compression level '" << val << "'\n";
				return 1;
			}
			argOffset += 2;
		}
		else if (arg == "import-materials")
		{
			if (argc < argOffset + 2)
			{
				std::cerr << "Usage: AssetPacker import-materials <source-dir>\n";
				return 1;
			}
			const int result = MaterialImporter::ImportDirectory(fs::path(argv[argOffset + 1]));
			return (result < 0) ? 1 : 0;
		}
		else
		{
			break; // positional args start here
		}
	}

	if (argc < argOffset + 2)
	{
		std::cerr << "Usage: AssetPacker [--import-materials] [--compress-level N] <source-dir> <output.pak>\n";
		std::cerr << "       AssetPacker import-materials <source-dir>\n";
		return 1;
	}

	const fs::path sourceDir(argv[argOffset]);
	const fs::path outPath(argv[argOffset + 1]);

	if (!fs::is_directory(sourceDir))
	{
		std::cerr << "AssetPacker: source directory not found: " << sourceDir << "\n";
		return 1;
	}

	if (importMaterials)
	{
		if (MaterialImporter::ImportDirectory(sourceDir) < 0)
			return 1;
	}

	PakWriter writer(compressionLevel);
	writer.AddDirectory(sourceDir);
	return writer.Write(outPath) ? 0 : 1;
}
