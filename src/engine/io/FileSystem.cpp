#include "FileSystem.hpp"

#include <map>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <thread>
#include <utility>

#include "DirectoryBackend.hpp"
#include "IFileBackend.hpp"
#include "IOThread.hpp"
#include "LogCategory.hpp"
#include "Logger.hpp"
#include "MeowExceptions.hpp"

namespace meow::io
{
	namespace
	{
		struct FileSystemBackend
		{
			// std::less<> enables heterogeneous lookup so string_view keys work without allocation.
			std::map<std::string, std::unique_ptr<IFileBackend>, std::less<>> mounts;
			std::unique_ptr<IOThread> ioThread;
		};

		FileSystemBackend* s_backend = nullptr;

		// Splits "mountpoint://relative/path" → { "mountpoint", "relative/path" }
		std::pair<std::string_view, std::string_view> ParseVirtualPath(std::string_view virtualPath)
		{
			constexpr std::string_view separator = "://";
			const auto sep = virtualPath.find(separator);
			if (sep == std::string_view::npos)
			{
				throw FileSystemError("Invalid virtual path (missing ://): " + std::string(virtualPath));
			}
			return { virtualPath.substr(0, sep), virtualPath.substr(sep + separator.size()) };
		}

		IFileBackend& ResolveBackend(std::string_view mountPoint)
		{
			auto it = s_backend->mounts.find(mountPoint);
			if (it == s_backend->mounts.end())
			{
				throw FileSystemError("No backend mounted at: " + std::string(mountPoint));
			}
			return *it->second;
		}

		std::filesystem::path ResolveMountedDirectory(const std::initializer_list<std::filesystem::path>& candidates)
		{
			for (const auto& candidate : candidates)
			{
				std::error_code errorCode;
				if (std::filesystem::exists(candidate, errorCode))
				{
					return candidate;
				}
			}

			return *candidates.begin();
		}
	}

	void FileSystem::Initialize()
	{
		if (s_backend != nullptr)
		{
			WARN(LogCategory::FileSystem, "FileSystem::Initialize() called more than once — ignoring.");
			return;
		}

		s_backend = new FileSystemBackend();
		s_backend->ioThread = std::make_unique<IOThread>();
		INFO(LogCategory::FileSystem, "FileSystem initialized.");
	}

	void FileSystem::InitializeDefaultMounts()
	{
		Initialize();

		const auto workingDirectory = std::filesystem::current_path();
		const auto assetsDirectory = ResolveMountedDirectory({
			workingDirectory / "assets",
			workingDirectory / "../assets",
			workingDirectory / "../../assets",
			});
		const auto shaderDirectory = ResolveMountedDirectory({
			workingDirectory / "shaders",
			workingDirectory / "build/shaders",
			workingDirectory / "../shaders",
			workingDirectory / "../../shaders",
			workingDirectory / "../build/shaders",
			workingDirectory / "../../build/shaders",
			});
		const auto logsDirectory = workingDirectory / "logs";

		Mount("assets", assetsDirectory);
		Mount("shaders", shaderDirectory);
		Mount("logs", logsDirectory);
	}

	void FileSystem::Shutdown()
	{
		if (s_backend == nullptr)
		{
			return;
		}

		// Drain all pending IO before tearing down.
		s_backend->ioThread->Flush();
		s_backend->ioThread.reset();
		s_backend->mounts.clear();

		delete s_backend;
		s_backend = nullptr;

		INFO(LogCategory::FileSystem, "FileSystem shut down.");
	}

	void FileSystem::Mount(std::string_view mountPoint, std::filesystem::path physicalPath)
	{
		if (s_backend == nullptr)
		{
			throw FileSystemError("FileSystem::Mount() called before Initialize().");
		}

		INFO(LogCategory::FileSystem, "Mounting '{}://' -> '{}'", mountPoint, physicalPath.string());
		s_backend->mounts.insert_or_assign(std::string(mountPoint),
			std::make_unique<DirectoryBackend>(std::move(physicalPath)));
	}

	bool FileSystem::Exists(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			throw FileSystemError("FileSystem::Exists() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		return ResolveBackend(mountPoint).Exists(relativePath);
	}

	std::vector<std::byte> FileSystem::ReadFile(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			throw FileSystemError("FileSystem::ReadFile() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		VERBOSE(LogCategory::FileSystem, "ReadFile: {}", virtualPath);
		return ResolveBackend(mountPoint).Read(relativePath);
	}

	std::unique_ptr<std::istream> FileSystem::OpenStream(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			throw FileSystemError("FileSystem::OpenStream() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		VERBOSE(LogCategory::FileSystem, "OpenStream: {}", virtualPath);
		return ResolveBackend(mountPoint).OpenStream(relativePath);
	}

	std::vector<std::string> FileSystem::Glob(std::string_view virtualPattern, const FileGlobOptions& options)
	{
		if (s_backend == nullptr)
		{
			throw FileSystemError("FileSystem::Glob() called before Initialize().");
		}

		const auto [mountPoint, relativePattern] = ParseVirtualPath(virtualPattern);
		auto matches = ResolveBackend(mountPoint).Glob(relativePattern, options);
		VERBOSE(LogCategory::FileSystem, "Glob: '{}' returned {} result(s)", virtualPattern, matches.size());
		return matches;
	}

	FileRequestHandle FileSystem::RequestAsync(std::string_view virtualPath, IOPriority priority)
	{
		if (s_backend == nullptr)
		{
			throw FileSystemError("FileSystem::RequestAsync() called before Initialize().");
		}

		const std::string virtualPathString(virtualPath);
		auto handle = std::make_shared<FileRequest>();
		VERBOSE(LogCategory::FileSystem, "RequestAsync (priority={}): {}",
			static_cast<int>(priority), virtualPathString);

		s_backend->ioThread->Submit(priority, [handle, virtualPathString]()
			{
				try
				{
					handle->m_data = FileSystem::ReadFile(virtualPathString);
					handle->m_state.store(FileRequest::State::Complete, std::memory_order_release);
				}
				catch (const std::exception& e)
				{
					handle->m_error = e.what();
					handle->m_state.store(FileRequest::State::Failed, std::memory_order_release);
				}
			});

		return handle;
	}

	void FileSystem::WaitFor(const FileRequestHandle& handle)
	{
		while (handle->GetState() == FileRequest::State::Pending)
		{
			// Yield to avoid spinning at 100% on one core.
			std::this_thread::yield();
		}
	}

	void FileSystem::Flush()
	{
		if (s_backend == nullptr)
		{
			return;
		}
		s_backend->ioThread->Flush();
	}
}
