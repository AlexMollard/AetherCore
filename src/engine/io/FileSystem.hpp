#pragma once

#include <expected>
#include <filesystem>
#include <istream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "FileGlobOptions.hpp"
#include "FileRequest.hpp"
#include "utils/Expected.hpp"
#include "utils/coro/Task.hpp"

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
		[[nodiscard]] static bool IsMounted(std::string_view mountPoint);

		// Register a physical directory under a virtual mount point name.
		// Replaces any existing backend registered to that mount point.
		static void Mount(std::string_view mountPoint, std::filesystem::path physicalPath);

		// Register a compiled .pak file under a virtual mount point name.
		// Replaces any existing backend registered to that mount point.
		static void MountPak(std::string_view mountPoint, std::filesystem::path pakPath);

		[[nodiscard]] static bool Exists(std::string_view virtualPath);

		// Synchronous read - returns entire file contents.
		[[nodiscard]] static Expected<std::vector<std::byte>> ReadFile(std::string_view virtualPath);

		// Convenience: read file as text string.
		[[nodiscard]] static Expected<std::string> ReadFileText(std::string_view virtualPath);

		// Synchronous write - writes data to a virtual path.
		// Creates parent directories if needed.
		[[nodiscard]] static Expected<void> WriteFile(std::string_view virtualPath, std::span<const std::byte> data);

		// Convenience: write text string to a virtual path.
		[[nodiscard]] static Expected<void> WriteFileText(std::string_view virtualPath, std::string_view text);

		// Synchronous stream - caller owns the returned stream.
		[[nodiscard]] static Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view virtualPath);

		// Glob files under a mount point using wildcards.
		// Supported wildcards:
		//   *  matches within one path segment
		//   ?  matches one character within one segment
		//   ** matches across directory boundaries
		[[nodiscard]] static Expected<std::vector<std::string>> Glob(std::string_view virtualPattern, const FileGlobOptions& options = {});

		// Asynchronous read - returns immediately with a handle.
		// Poll FileRequestHandle::GetState() or call FileSystem::WaitFor() to
		// synchronise.
		[[nodiscard]] static FileRequestHandle RequestAsync(std::string_view virtualPath, IOPriority priority = IOPriority::Normal);

		// Block until the given async request has completed.
		static void WaitFor(const FileRequestHandle& handle);

		// Coroutine async read - returns a task that becomes ready when the
		// file has been read on the background I/O thread.  The calling
		// coroutine suspends without blocking the game thread.
		[[nodiscard]] static coro::task<std::vector<std::byte>> ReadFileAsync(std::string_view virtualPath, IOPriority priority = IOPriority::Normal);

		// Block until all outstanding async requests have completed.
		static void Flush();
	};
} // namespace aether::io
