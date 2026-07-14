#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "utils/Expected.hpp"

namespace aether::io::file_util
{
	[[nodiscard]] Expected<std::string> ReadText(const std::filesystem::path& path);

	[[nodiscard]] Expected<void> WriteText(const std::filesystem::path& path, std::string_view text);

	[[nodiscard]] Expected<std::vector<std::byte>> ReadBinary(const std::filesystem::path& path);

	[[nodiscard]] Expected<void> WriteBinary(const std::filesystem::path& path, std::span<const std::byte> data);

	[[nodiscard]] Expected<void> CreateDirectories(const std::filesystem::path& path);

	[[nodiscard]] Expected<void> CopyFile(const std::filesystem::path& from, const std::filesystem::path& to, bool overwrite = true);

	[[nodiscard]] bool Exists(const std::filesystem::path& path);

	[[nodiscard]] Expected<void> Remove(const std::filesystem::path& path);

	[[nodiscard]] Expected<uintmax_t> FileSize(const std::filesystem::path& path);
} // namespace aether::io::file_util
