#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	// Scans a directory tree for PBR texture folders and generates (or refreshes)
	// a properties.toml for each one.  Future expansion: add texture compression,
	// mip generation, or custom metadata injection through this namespace.
	namespace MaterialImporter
	{
		struct TextureFile
		{
			std::string filename;
			std::string stem;
			std::string normalizedStem;
			std::vector<std::string> tokens;
		};

		// Process every subdirectory under sourceDir, generating or refreshing a
		// properties.toml for any folder that contains image files.
		// Returns the number of files written, or -1 on error.
		int ImportDirectory(const fs::path& sourceDir);

		// Returns true if the given folder had a file generated/updated.
		bool GeneratePropertiesForFolder(const fs::path& folder);

		// Returns true if outPath does not yet exist or was previously auto-generated
		// and is safe to overwrite.
		bool ShouldWrite(const fs::path& outPath);

		std::string ToLowerAscii(std::string s);
		std::string NormalizeForMatch(std::string s);
		std::vector<std::string> TokenizeStem(const std::string& stem);
		bool IsImageExtension(const fs::path& path);
		bool ContainsAlias(const TextureFile& file, std::string_view alias);

		std::optional<std::string> FindBestTexture(
		        const std::vector<TextureFile>& files, std::initializer_list<std::string_view> includeAliases, std::initializer_list<std::string_view> preferredAliases = {}, std::initializer_list<std::string_view> excludeAliases = {});
	} // namespace MaterialImporter
} // namespace aether::assetpipeline
