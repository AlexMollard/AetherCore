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
	// Read entire text file. Returns nullopt if the file doesn't exist.
	[[nodiscard]] Expected<std::string> ReadText(const std::filesystem::path& path);

	// Write text to a file, creating parent directories if needed.
	[[nodiscard]] Expected<void> WriteText(const std::filesystem::path& path, std::string_view text);

	// Read entire binary file. Returns nullopt if the file doesn't exist.
	[[nodiscard]] Expected<std::vector<std::byte>> ReadBinary(const std::filesystem::path& path);

	// Write binary data to a file, creating parent directories if needed.
	[[nodiscard]] Expected<void> WriteBinary(const std::filesystem::path& path, std::span<const std::byte> data);

	// Create a directory (and any missing parents). Succeeds if already exists.
	[[nodiscard]] Expected<void> CreateDirectories(const std::filesystem::path& path);

	// Copy a file. Fails if the destination exists and overwrite is false.
	[[nodiscard]] Expected<void> CopyFile(const std::filesystem::path& from, const std::filesystem::path& to, bool overwrite = true);

	// Check if a path exists.
	[[nodiscard]] bool Exists(const std::filesystem::path& path);

	// Delete a file or empty directory.
	[[nodiscard]] Expected<void> Remove(const std::filesystem::path& path);

	// Get file size in bytes. Returns error if the path doesn't exist.
	[[nodiscard]] Expected<uintmax_t> FileSize(const std::filesystem::path& path);
} // namespace aether::io::file_util
