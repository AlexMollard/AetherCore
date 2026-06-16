#pragma once

#include "daScript/misc/platform.h"
#include "daScript/simulate/debug_info.h"
#include "daScript/simulate/fs_file_info.h"
#include "daScript/ast/ast.h"

#include <string>
#include <vector>

namespace aether::app::scripting
{
	// Routes daScript file opens through the engine's VFS (FileSystem::ReadFile).
	// Falls back to the native filesystem for paths outside any mount (e.g. daslib/).
	class VfsFileSystem : public das::AnyFileSystem
	{
	public:
		das::FileInfo* tryOpenFile(const das::string& fileName) override;
	};

	// Custom FileAccess that:
	// 1. Uses VfsFileSystem for file opens (engine VFS -> native FS fallback)
	// 2. Registers extraRoots for module resolution (systems/ -> scripts://systems/)
	class VfsFileAccess : public das::FsFileAccess
	{
	public:
		VfsFileAccess();

		// Add a search root: require "prefix/module" tries "rootPath/module.das"
		void AddSearchRoot(const das::string& prefix, const das::string& rootPath);

		das::ModuleInfo getModuleInfo(const das::string& req, const das::string& from) const override;

	private:
		struct SearchRoot
		{
			das::string prefix;
			das::string rootPath;
		};

		mutable std::vector<SearchRoot> searchRoots;
	};

} // namespace aether::app::scripting
