#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "PakFormat.hpp"

namespace aether::assetpipeline
{
	namespace fs = std::filesystem;

	class PakWriter
	{
	public:
		explicit PakWriter(int compressionLevel = 3)
		      : m_compressionLevel(compressionLevel)
		{
		}

		void AddDirectory(const fs::path& sourceDir);

		// ship (no project-layout exclusion rules apply otherwise).
		void AddDirectoryAs(const fs::path& sourceDir, std::string_view vpathPrefix);

		bool Write(const fs::path& outPath) const;

		std::size_t FileCount() const
		{
			return m_files.size();
		}

		struct FileRecord
		{
			std::string virtualPath;
			fs::path diskPath;
			fs::path sourceDir;
		};

	private:
		std::vector<FileRecord> m_files;
		fs::path m_sourceDir;
		int m_compressionLevel;
	};
} // namespace aether::assetpipeline
