#pragma once

#include <filesystem>

#include "IFileBackend.hpp"

namespace aether::io
{
	class DirectoryBackend final : public IFileBackend
	{
	public:
		explicit DirectoryBackend(std::filesystem::path rootPath);

		[[nodiscard]] bool Exists(std::string_view relativePath) const override;
		[[nodiscard]] std::vector<std::byte> Read(std::string_view relativePath) const override;
		[[nodiscard]] std::unique_ptr<std::istream> OpenStream(std::string_view relativePath) const override;
		[[nodiscard]] std::vector<std::string> Glob(std::string_view pattern, const FileGlobOptions& options) const override;

	private:
		[[nodiscard]] std::filesystem::path Resolve(std::string_view relativePath) const;

		std::filesystem::path m_rootPath;
	};
} // namespace aether::io
