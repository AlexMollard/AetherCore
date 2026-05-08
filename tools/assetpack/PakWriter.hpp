#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <PakFormat.hpp>

namespace fs = std::filesystem;

// Collects files from a source directory and writes them into a single binary
// .pak file consumed by the runtime PakBackend.
//
// Files are read and compressed in parallel, then written deterministically
// (sorted by virtual path).  Pass compressionLevel = 0 to disable compression.
class PakWriter
{
public:
	explicit PakWriter(int compressionLevel = 3) : m_compressionLevel(compressionLevel) {}

	// Add all regular files under sourceDir to the archive.  Virtual paths
	// are relative to sourceDir with forward-slash separators.
	void AddDirectory(const fs::path& sourceDir);

	// Write the collected entries to outPath.  Returns true on success.
	bool Write(const fs::path& outPath) const;

	std::size_t FileCount() const
	{
		return m_files.size();
	}

	// Exposed so manifest helpers in PakWriter.cpp can inspect the file list.
	struct FileRecord
	{
		std::string virtualPath; // relative to sourceDir, forward-slash separated
		fs::path    diskPath;
	};

private:
	std::vector<FileRecord> m_files;
	fs::path                m_sourceDir;
	int                     m_compressionLevel;
};
