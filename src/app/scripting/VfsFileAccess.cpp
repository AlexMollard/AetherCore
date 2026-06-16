#include "VfsFileAccess.hpp"

#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"
#include "utils/LogCategory.hpp"

#include <filesystem>
#include <fstream>
#include <utility>

namespace aether::app::scripting
{
	// -- VfsFileSystem -------------------------------------------------------------

	das::FileInfo* VfsFileSystem::tryOpenFile(const das::string& fileName)
	{
		// Try engine VFS first (paths containing ://)
		if (fileName.find("://") != das::string::npos)
		{
			if (io::FileSystem::IsInitialized())
			{
				auto result = io::FileSystem::ReadFile(fileName);
				if (result.has_value())
				{
					auto& data = result.value();
					if (data.empty())
					{
						return new das::TextFileInfo("", 0, false);
					}
					char* source = static_cast<char*>(das_aligned_alloc16(data.size()));
					std::memcpy(source, data.data(), data.size());
					return new das::TextFileInfo(source, static_cast<uint32_t>(data.size()), true);
				}
			}
			return nullptr;
		}

		// Fall back to native filesystem for absolute/relative paths
		// This is needed for daslib and daScript's own modules
		if (FILE* ff = fopen(fileName.c_str(), "rb"))
		{
			struct stat st;
			int fd = fileno(ff);
			fstat(fd, &st);
			if (!st.st_size)
			{
				fclose(ff);
				return new das::TextFileInfo("", st.st_size, false);
			}
			char* source = static_cast<char*>(das_aligned_alloc16(st.st_size));
			auto info = new das::TextFileInfo(source, st.st_size, true);
			auto bytesRead = fread(source, 1, st.st_size, ff);
			fclose(ff);
			if (std::cmp_equal(bytesRead, st.st_size))
			{
				return info;
			}
			else
			{
				delete info;
				return nullptr;
			}
		}

		return nullptr;
	}

	// -- VfsFileAccess -------------------------------------------------------------

	VfsFileAccess::VfsFileAccess()

	{
		// Insert VfsFileSystem at the front so it is tried first.
		auto vfs = new VfsFileSystem();
		addFileSystem(vfs, true, false);

		// Introduce daslib modules (json, strings, etc.) from the daScript installation.
		// This pre-loads them into the file info cache so they're available when
		// require "daslib/xxx" is processed.
		introduceDaslib();
		introduceNativeModules();
	}

	void VfsFileAccess::AddSearchRoot(const das::string& prefix, const das::string& rootPath)
	{
		searchRoots.push_back({.prefix = prefix, .rootPath = rootPath});
		addFsRoot(prefix, rootPath);
	}

	das::ModuleInfo VfsFileAccess::getModuleInfo(const das::string& req, const das::string& from) const
	{
		// Check our custom search roots first.
		auto np = req.find_first_of("./");
		if (np != das::string::npos)
		{
			das::string top = req.substr(0, np);
			auto last_np = req.find_last_of("./");
			das::string path = last_np == np ? "" : req.substr(np + 1, last_np - np - 1);
			das::string mod_name = req.substr(last_np + 1);

			for (const auto& root: searchRoots)
			{
				if (top == root.prefix)
				{
					das::ModuleInfo info;
					info.moduleName = mod_name;
					if (!path.empty())
					{
						info.fileName = root.rootPath + "/" + path + "/" + mod_name + ".das";
					}
					else
					{
						info.fileName = root.rootPath + "/" + mod_name + ".das";
					}
					return info;
				}
			}
		}

		// Fall back to the default resolution (daslib, native modules, etc.)
		// Call parent implementation which handles daslib, native modules, and extraRoots
		return das::FsFileAccess::getModuleInfo(req, from);
	}

} // namespace aether::app::scripting
