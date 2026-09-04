#pragma once

#include <expected>
#include <filesystem>
#include <span>

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
		[[nodiscard]] Expected<void> Write(std::string_view relativePath, std::span<const std::byte> data) const override;

	private:
		[[nodiscard]] std::optional<std::filesystem::path> Resolve(std::string_view relativePath) const;

		[[nodiscard]] std::optional<std::filesystem::path> ResolveInsensitive(std::string_view relativePath, std::string* bestMatch = nullptr) const;

		[[nodiscard]] std::vector<std::string> CollectDidYouMean(std::string_view relativePath, int maxSuggestions = 3) const;

		std::filesystem::path m_rootPath;
	};
} // namespace aether::io
