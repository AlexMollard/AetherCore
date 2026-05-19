#include "FileSystem.hpp"

#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "utils/AetherExceptions.hpp"
#include "utils/Assert.hpp"
#include "utils/Expected.hpp"
#include "DirectoryBackend.hpp"
#include "IFileBackend.hpp"
#include "IOThread.hpp" // IoExecutor
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "PakBackend.hpp"

namespace aether::io
{
	namespace
	{
		struct FileSystemBackend
		{
			// std::less<> enables heterogeneous lookup so string_view keys work without
			// allocation.
			std::map<std::string, std::shared_ptr<IFileBackend>, std::less<>> mounts;
			std::mutex mountsMutex;
			std::unique_ptr<IoExecutor> ioThread;
		};

		FileSystemBackend* s_backend = nullptr;

		// Splits "mountpoint://relative/path" -> { "mountpoint", "relative/path" }
		std::pair<std::string_view, std::string_view> ParseVirtualPath(std::string_view virtualPath)
		{
			constexpr std::string_view separator = "://";
			const auto sep = virtualPath.find(separator);
			if (sep == std::string_view::npos)
			{
				AE_ASSERT_ALWAYS(false, "Invalid virtual path (missing ://): " + std::string(virtualPath));
			}
			return { virtualPath.substr(0, sep), virtualPath.substr(sep + separator.size()) };
		}

		std::shared_ptr<IFileBackend> ResolveBackend(std::string_view mountPoint)
		{
			std::scoped_lock lock(s_backend->mountsMutex);
			auto it = s_backend->mounts.find(mountPoint);
			if (it == s_backend->mounts.end())
			{
				AE_ASSERT_ALWAYS(false, "No backend mounted at: " + std::string(mountPoint));
			}
			return it->second;
		}

		std::filesystem::path ResolveMountedDirectory(const std::initializer_list<std::filesystem::path>& candidates)
		{
			for (const auto& candidate: candidates)
			{
				std::error_code errorCode;
				if (std::filesystem::exists(candidate, errorCode))
				{
					return candidate;
				}
			}

			return *candidates.begin();
		}
	} // namespace

	void FileSystem::Initialize()
	{
		if (s_backend != nullptr)
		{
			AE_WARN(LogCategory::FileSystem, "FileSystem::Initialize() called more than once - ignoring.");
			return;
		}

		s_backend = new FileSystemBackend();
		s_backend->ioThread = std::make_unique<IoExecutor>();
		AE_INFO(LogCategory::FileSystem, "FileSystem initialized.");
	}

	bool FileSystem::IsInitialized()
	{
		return s_backend != nullptr;
	}

	void FileSystem::InitializeDefaultMounts()
	{
		Initialize();

		const auto workingDirectory = std::filesystem::current_path();

		// ── assets:// ─────────────────────────────────────────────────────────
		// Prefer a compiled pak produced by AssetPacker at build time.
		// Fall back to a loose assets/ directory if the pak does not yet exist
		// (e.g., clean checkout before first build).
		const std::filesystem::path pakCandidates[] = {
			workingDirectory / "data" / "assets.pak",
			workingDirectory / "../data/assets.pak",
			workingDirectory / "../../data/assets.pak",
		};
		bool pakMounted = false;
		for (const auto& candidate: pakCandidates)
		{
			std::error_code ec;
			if (std::filesystem::exists(candidate, ec))
			{
				MountPak("assets", candidate);
				pakMounted = true;
				break;
			}
		}
		if (!pakMounted)
		{
			const auto assetsDirectory = ResolveMountedDirectory({
			        workingDirectory / "assets",
			        workingDirectory / "../assets",
			        workingDirectory / "../../assets",
			});
			Mount("assets", assetsDirectory);
		}

		// ── shaders:// ────────────────────────────────────────────────────────
		const auto shaderDirectory = ResolveMountedDirectory({
		        workingDirectory / "shaders",
		        workingDirectory / "build/shaders",
		        workingDirectory / "../shaders",
		        workingDirectory / "../../shaders",
		        workingDirectory / "../build/shaders",
		        workingDirectory / "../../build/shaders",
		});
		Mount("shaders", shaderDirectory);

		// ── config:// ─────────────────────────────────────────────────────────
		// Settings/config files are deployed to data/config at build time.
		const auto configDirectory = ResolveMountedDirectory({
		        workingDirectory / "data/config",
		        workingDirectory / "../data/config",
		        workingDirectory / "../../data/config",
		        workingDirectory / "config",
		        workingDirectory / "../config",
		        workingDirectory / "../../config",
		});
		Mount("config", configDirectory);

		// ── logs:// ───────────────────────────────────────────────────────────
		Mount("logs", workingDirectory / "logs");
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
		{
			std::scoped_lock lock(s_backend->mountsMutex);
			s_backend->mounts.clear();
		}

		delete s_backend;
		s_backend = nullptr;

		AE_INFO(LogCategory::FileSystem, "FileSystem shut down.");
	}

	void FileSystem::Mount(std::string_view mountPoint, std::filesystem::path physicalPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::Mount() called before Initialize().");
		}

		AE_INFO(LogCategory::FileSystem, "Mounting '{}://' -> '{}'", mountPoint, physicalPath.string());
		std::scoped_lock lock(s_backend->mountsMutex);
		s_backend->mounts.insert_or_assign(std::string(mountPoint), std::make_shared<DirectoryBackend>(std::move(physicalPath)));
	}

	void FileSystem::MountPak(std::string_view mountPoint, std::filesystem::path pakPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::MountPak() called before Initialize().");
		}

		AE_INFO(LogCategory::FileSystem, "Mounting pak '{}://' -> '{}'", mountPoint, pakPath.string());
		std::scoped_lock lock(s_backend->mountsMutex);
		s_backend->mounts.insert_or_assign(std::string(mountPoint), std::make_shared<PakBackend>(std::move(pakPath)));
	}

	bool FileSystem::Exists(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::Exists() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		const auto backend = ResolveBackend(mountPoint);
		return backend->Exists(relativePath);
	}

	Expected<std::vector<std::byte>> FileSystem::ReadFile(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::ReadFile() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		AE_VERBOSE(LogCategory::FileSystem, "ReadFile: {}", virtualPath);
		const auto backend = ResolveBackend(mountPoint);
		return backend->Read(relativePath);
	}

	Expected<std::unique_ptr<std::istream>> FileSystem::OpenStream(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::OpenStream() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		AE_VERBOSE(LogCategory::FileSystem, "OpenStream: {}", virtualPath);
		const auto backend = ResolveBackend(mountPoint);
		return backend->OpenStream(relativePath);
	}

	Expected<std::vector<std::string>> FileSystem::Glob(std::string_view virtualPattern, const FileGlobOptions& options)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::Glob() called before Initialize().");
		}

		const auto [mountPoint, relativePattern] = ParseVirtualPath(virtualPattern);
		const auto backend = ResolveBackend(mountPoint);
		auto result = backend->Glob(relativePattern, options);
		if (!result.has_value())
		{
			return result;
		}
		AE_VERBOSE(LogCategory::FileSystem, "Glob: '{}' returned {} result(s)", virtualPattern, result->size());
		return result;
	}

	FileRequestHandle FileSystem::RequestAsync(std::string_view virtualPath, IOPriority priority)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::RequestAsync() called before Initialize().");
		}

		const std::string virtualPathString(virtualPath);
		auto handle = std::make_shared<FileRequest>();
		AE_VERBOSE(LogCategory::FileSystem, "RequestAsync (priority={}): {}", static_cast<int>(priority), virtualPathString);

		s_backend->ioThread->Submit(priority,
		        [handle, virtualPathString]()
		        {
			        auto result = FileSystem::ReadFile(virtualPathString);
			        if (result.has_value())
			        {
				        handle->m_data = std::move(*result);
				        handle->m_state.store(FileRequest::State::Complete, std::memory_order_release);
			        }
			        else
			        {
				        handle->m_error = result.error().ToString();
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

	coro::task<std::vector<std::byte>> FileSystem::ReadFileAsync(std::string_view virtualPath, IOPriority priority)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::ReadFileAsync() called before Initialize().");
		}

		auto pair = coro::task<std::vector<std::byte>>::create();
		const std::string pathStr(virtualPath);

		s_backend->ioThread->Submit(priority,
		        [pathStr, source = std::move(pair.second)]() mutable
		        {
			        auto result = FileSystem::ReadFile(pathStr);
			        if (result.has_value())
			        {
				        source.set_value(std::move(*result));
			        }
			        else
			        {
				        source.set_exception(std::make_exception_ptr(AssetError(result.error().ToString())));
			        }
		        });

		return std::move(pair.first);
	}

	void FileSystem::Flush()
	{
		if (s_backend == nullptr)
		{
			return;
		}
		s_backend->ioThread->Flush();
	}
} // namespace aether::io
