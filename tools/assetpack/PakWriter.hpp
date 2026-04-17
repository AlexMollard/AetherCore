#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "PakFormat.hpp"

namespace fs = std::filesystem;

// Collects files from one or more source directories and writes them into a
// single binary .pak file consumed by the runtime PakBackend.
//
// Future extension points:
//   - Asset processors (compression, texture transcoding, etc.) can be added
//     as a processing step in AddDirectory before the raw bytes are committed.
//   - Per-entry metadata or flags can be added here before the format is
//     extended in PakFormat.hpp.
class PakWriter
{
public:
	// Add all regular files under sourceDir to the archive.  Virtual paths
	// are relative to sourceDir with forward-slash separators.
	void AddDirectory(const fs::path& sourceDir);

	// Write the collected entries to outPath.  Returns true on success.
	bool Write(const fs::path& outPath) const;

	std::size_t FileCount() const
	{
		return m_files.size();
	}

private:
	struct FileRecord
	{
		std::string virtualPath; // relative to sourceDir, forward-slash separated
		fs::path diskPath;
	};

	std::vector<FileRecord> m_files;
};
