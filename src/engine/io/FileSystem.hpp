#pragma once

#include <filesystem>
#include <istream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "FileGlobOptions.hpp"
#include "FileRequest.hpp"

namespace aether::io
{
	class FileSystem
	{
	public:
		FileSystem() = delete;

		static void Initialize();
		static void InitializeDefaultMounts();
		static void Shutdown();
		[[nodiscard]] static bool IsInitialized();

		// Register a physical directory under a virtual mount point name.
		// Replaces any existing backend registered to that mount point.
		static void Mount(std::string_view mountPoint, std::filesystem::path physicalPath);

		// Register a compiled .pak file under a virtual mount point name.
		// Replaces any existing backend registered to that mount point.
		static void MountPak(std::string_view mountPoint, std::filesystem::path pakPath);

		[[nodiscard]] static bool Exists(std::string_view virtualPath);

		// Synchronous read — returns entire file contents. Fine for startup / shader
		// loading.
		[[nodiscard]] static std::vector<std::byte> ReadFile(std::string_view virtualPath);

		// Synchronous stream — caller owns the returned stream.
		[[nodiscard]] static std::unique_ptr<std::istream> OpenStream(std::string_view virtualPath);

		// Glob files under a mount point using wildcards.
		// Supported wildcards:
		//   *  matches within one path segment
		//   ?  matches one character within one segment
		//   ** matches across directory boundaries
		[[nodiscard]] static std::vector<std::string> Glob(std::string_view virtualPattern, const FileGlobOptions& options = {});

		// Asynchronous read — returns immediately with a handle.
		// Poll FileRequestHandle::GetState() or call FileSystem::WaitFor() to
		// synchronise.
		[[nodiscard]] static FileRequestHandle RequestAsync(std::string_view virtualPath, IOPriority priority = IOPriority::Normal);

		// Block until the given async request has completed.
		static void WaitFor(const FileRequestHandle& handle);

		// Block until all outstanding async requests have completed.
		static void Flush();
	};
} // namespace aether::io
