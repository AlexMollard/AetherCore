#include "AssetPipeline.hpp"

#include <system_error>

#include "MaterialImporter.hpp"
#include "PakWriter.hpp"

namespace aether::assetpipeline
{
	namespace
	{
		bool HasProjectDescriptor(const std::filesystem::path& root)
		{
			std::error_code ec;
			return std::filesystem::is_regular_file(root / "ProjectSettings.toml", ec);
		}

		PackResult RunPack(const std::filesystem::path& sourceDir, const std::filesystem::path& outputPak, const PackOptions& options)
		{
			std::error_code ec;
			if (!std::filesystem::is_directory(sourceDir, ec))
			{
				return {.ok = false, .message = "Source directory not found: " + sourceDir.generic_string(), .outputPath = outputPak};
			}

			if (options.importMaterials)
			{
				const std::filesystem::path materialRoot =
				        (options.projectLayout && std::filesystem::is_directory(sourceDir / "assets", ec)) ? sourceDir / "assets" : sourceDir;
				if (MaterialImporter::ImportDirectory(materialRoot) < 0)
				{
					return {.ok = false, .message = "Material import failed for: " + materialRoot.generic_string(), .outputPath = outputPak};
				}
			}

			PakWriter writer(options.compressionLevel);
			writer.AddDirectory(sourceDir);

			if (!options.shaderSpirvDir.empty() && std::filesystem::is_directory(options.shaderSpirvDir, ec))
			{
				writer.AddDirectoryAs(options.shaderSpirvDir, "shaders");
			}

			const std::uint64_t sourceFiles = static_cast<std::uint64_t>(writer.FileCount());

			if (!writer.Write(outputPak))
			{
				return {.ok = false,
				        .message = "Pack failed; see " + std::filesystem::path(outputPak.string() + ".log").generic_string(),
				        .sourceFiles = sourceFiles,
				        .outputPath = outputPak};
			}

			std::uintmax_t bytes = std::filesystem::file_size(outputPak, ec);
			if (ec)
			{
				bytes = 0;
			}
			return {.ok = true,
			        .message = "Packed " + std::to_string(sourceFiles) + " file(s) (" + std::to_string(bytes / 1024) + " KB).",
			        .sourceFiles = sourceFiles,
			        .pakBytes = bytes,
			        .outputPath = outputPak};
		}
	} // namespace

	PackResult PackProject(const std::filesystem::path& projectRoot, const std::filesystem::path& outputPak, const PackOptions& options)
	{
		if (options.projectLayout && !HasProjectDescriptor(projectRoot))
		{
			return {.ok = false, .message = "Project descriptor is missing: " + (projectRoot / "ProjectSettings.toml").generic_string(), .outputPath = outputPak};
		}
		return RunPack(projectRoot, outputPak, options);
	}

	PackResult PackDirectory(const std::filesystem::path& sourceDir, const std::filesystem::path& outputPak, const PackOptions& options)
	{
		return RunPack(sourceDir, outputPak, options);
	}
}
