#pragma once

#include <expected>
#include <filesystem>

#include "IFileBackend.hpp"
#include "utils/Expected.hpp"

namespace aether::io
{
	class DirectoryBackend final : public IFileBackend
	{
	public:
		explicit DirectoryBackend(std::filesystem::path rootPath);

		[[nodiscard]] bool Exists(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::byte>> Read(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view relativePath) const override;
		[[nodiscard]] Expected<std::vector<std::string>> Glob(std::string_view pattern, const FileGlobOptions& options) const override;

	private:
		[[nodiscard]] std::filesystem::path Resolve(std::string_view relativePath) const;

		// Resolve a relative path to an actual filesystem path.  Tries the exact
		// path first, then falls back to a case-insensitive scan of the parent
		// directory.  Returns std::nullopt if nothing is found.
		// If bestMatch is non-null and only a case-insensitive match exists, the
		// correctly-cased virtual path is written there.
		[[nodiscard]] std::optional<std::filesystem::path> ResolveInsensitive(std::string_view relativePath, std::string* bestMatch = nullptr) const;

		// Collect up to maxSuggestions filenames in the same parent directory
		// that are Levenshtein-close to the last component of relativePath.
		[[nodiscard]] std::vector<std::string> CollectDidYouMean(std::string_view relativePath, int maxSuggestions = 3) const;

		std::filesystem::path m_rootPath;
	};
} // namespace aether::io
