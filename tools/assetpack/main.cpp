// AssetPacker — build-time tool that bundles a source-asset directory into a
// single binary (.pak) file consumed by the runtime PakBackend.
//
// Usage:
//   AssetPacker [--import-materials] <source-dir> <output.pak>
//   AssetPacker import-materials <source-dir>

#include <filesystem>
#include <iostream>
#include <string>

#include "MaterialImporter.hpp"
#include "PakWriter.hpp"

namespace fs = std::filesystem;

int main(int argc, char* argv[])
{
	bool importMaterials = false;
	int argOffset = 1;

	if (argc >= 2)
	{
		const std::string firstArg = argv[1];
		if (firstArg == "--import-materials")
		{
			importMaterials = true;
			argOffset = 2;
		}
		else if (firstArg == "import-materials")
		{
			if (argc < 3)
			{
				std::cerr << "Usage: AssetPacker import-materials <source-dir>\n";
				return 1;
			}
			const int result = MaterialImporter::ImportDirectory(fs::path(argv[2]));
			return (result < 0) ? 1 : 0;
		}
	}

	if (argc < argOffset + 2)
	{
		std::cerr << "Usage: AssetPacker [--import-materials] <source-dir> <output.pak>\n";
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
		{
			return 1;
		}
	}

	PakWriter writer;
	writer.AddDirectory(sourceDir);
	return writer.Write(outPath) ? 0 : 1;
}
