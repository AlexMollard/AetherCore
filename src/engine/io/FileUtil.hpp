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

	// Moves a file or directory to the OS recycle bin / trash, so a mistaken delete in the
	// editor is recoverable the way every other application on the machine behaves.
	//
	// Returns false when the platform has no trash support compiled in, rather than deleting
	// anything: the caller decides whether to fall back to a permanent Remove, and can say so
	// in the confirmation it shows.
	[[nodiscard]] bool MoveToTrash(const std::filesystem::path& path);

	[[nodiscard]] Expected<uintmax_t> FileSize(const std::filesystem::path& path);
} // namespace aether::io::file_util
