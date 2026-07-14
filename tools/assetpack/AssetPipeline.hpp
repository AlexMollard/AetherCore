#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace aether::assetpipeline
{
	struct PackOptions
	{
		int compressionLevel = 3;
		bool importMaterials = false; // run MaterialImporter before packing
		bool projectLayout = false;

		std::filesystem::path shaderSpirvDir;
	};

	struct PackResult
	{
		bool ok = false;
		std::string message;
		std::uint64_t sourceFiles = 0;
		std::uintmax_t pakBytes = 0;
		std::filesystem::path outputPath;
	};

	[[nodiscard]] PackResult PackProject(const std::filesystem::path& projectRoot, const std::filesystem::path& outputPak, const PackOptions& options);

	[[nodiscard]] PackResult PackDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& outputPak, const PackOptions& options);
} // namespace aether::assetpipeline
