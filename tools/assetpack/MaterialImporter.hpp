#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	namespace MaterialImporter
	{
		struct TextureFile
		{
			std::string filename;
			std::string stem;
			std::string normalizedStem;
			std::vector<std::string> tokens;
		};

		int ImportDirectory(const fs::path& sourceDir);

		bool GeneratePropertiesForFolder(const fs::path& folder);

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
