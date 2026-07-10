#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "PakFormat.hpp"

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	// Collects files from a source directory and writes them into a single binary
	// .pak file consumed by the runtime PakBackend.
	//
	// Files are read and compressed in parallel, then written deterministically
	// (sorted by virtual path).  Pass compressionLevel = 0 to disable compression.
	class PakWriter
	{
	public:
		explicit PakWriter(int compressionLevel = 3)
		      : m_compressionLevel(compressionLevel)
		{
		}

		// Add all regular files under sourceDir to the archive.  Virtual paths
		// are relative to sourceDir with forward-slash separators.  Applies the
		// project-pack exclusion rules (Builds/, .git/, ProjectSettings.toml,
		// *.slang source shaders, etc).
		void AddDirectory(const fs::path& sourceDir);

		// Add every ".spv" file under sourceDir to the archive with its virtual
		// path prefixed by vpathPrefix (e.g. prefix "shaders" turns
		// "<sourceDir>/x.spv" into the pak entry "shaders/x.spv"). Intended for
		// a compiled-shader intermediate dir (ShaderCompiler's output), which
		// also holds per-shader ".slangc.log" build logs alongside the .spv -
		// those and anything else non-.spv are skipped so only shader binaries
		// ship (no project-layout exclusion rules apply otherwise).
		void AddDirectoryAs(const fs::path& sourceDir, std::string_view vpathPrefix);

		// Write the collected entries to outPath.  Returns true on success.
		bool Write(const fs::path& outPath) const;

		std::size_t FileCount() const
		{
			return m_files.size();
		}

		// Exposed so manifest helpers in PakWriter.cpp can inspect the file list.
		struct FileRecord
		{
			std::string virtualPath; // pak-relative, forward-slash separated (may include a prefix from AddDirectoryAs)
			fs::path diskPath;
			fs::path sourceDir; // directory virtualPath (minus any prefix) is relative to; passed to ProcessAsset for companion-file resolution
		};

	private:
		std::vector<FileRecord> m_files;
		fs::path m_sourceDir;
		int m_compressionLevel;
	};
} // namespace aether::assetpipeline
