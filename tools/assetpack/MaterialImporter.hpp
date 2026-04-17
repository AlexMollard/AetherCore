#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// Scans a directory tree for PBR texture folders and generates (or refreshes)
// a properties.toml for each one.  Future expansion: add texture compression,
// mip generation, or custom metadata injection through this class.
class MaterialImporter
{
public:
	// Process every subdirectory under sourceDir, generating or refreshing a
	// properties.toml for any folder that contains image files.
	// Returns the number of files written, or -1 on error.
	static int ImportDirectory(const fs::path& sourceDir);

private:
	// Returns true if the given folder had a file generated/updated.
	static bool GeneratePropertiesForFolder(const fs::path& folder);

	// Returns true if outPath does not yet exist or was previously auto-generated
	// and is safe to overwrite.
	static bool ShouldWrite(const fs::path& outPath);

	// -----------------------------------------------------------------------
	// Texture-matching helpers
	// -----------------------------------------------------------------------

	struct TextureFile
	{
		std::string filename;
		std::string stem;
		std::string normalizedStem;
		std::vector<std::string> tokens;
	};

	static std::string ToLowerAscii(std::string s);
	static std::string NormalizeForMatch(std::string s);
	static std::vector<std::string> TokenizeStem(const std::string& stem);
	static bool IsImageExtension(const fs::path& path);

	static bool ContainsAlias(const TextureFile& file, std::string_view alias);

	static std::optional<std::string> FindBestTexture(const std::vector<TextureFile>& files, std::initializer_list<std::string_view> includeAliases, std::initializer_list<std::string_view> preferredAliases = {}, std::initializer_list<std::string_view> excludeAliases = {});
};
