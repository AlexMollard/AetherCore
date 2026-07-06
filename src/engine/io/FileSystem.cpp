#include "FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "utils/AetherExceptions.hpp"
#include "utils/Profiler.hpp"
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
			return {virtualPath.substr(0, sep), virtualPath.substr(sep + separator.size())};
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

		bool PathExists(const std::filesystem::path& path)
		{
			std::error_code ec;
			return std::filesystem::exists(path, ec);
		}

		std::filesystem::path NormalPath(std::filesystem::path path)
		{
			std::error_code ec;
			auto absolute = std::filesystem::absolute(path, ec);
			if (!ec)
			{
				path = std::move(absolute);
			}

			auto canonical = std::filesystem::weakly_canonical(path, ec);
			if (!ec)
			{
				return canonical;
			}
			return path.lexically_normal();
		}

		void AddUniquePath(std::vector<std::filesystem::path>& paths, std::filesystem::path path)
		{
			path = NormalPath(std::move(path));
			for (const auto& existing: paths)
			{
				if (existing == path)
				{
					return;
				}
			}
			paths.push_back(std::move(path));
		}

		std::string EnvironmentString(const char* name)
		{
#ifdef _MSC_VER
			char* value = nullptr;
			std::size_t size = 0;
			if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
			{
				return {};
			}
			std::string result(value);
			std::free(value);
			return result;
#else
			if (const char* value = std::getenv(name); value != nullptr)
			{
				return value;
			}
			return {};
#endif
		}

		std::optional<std::filesystem::path> EnvironmentPath(const char* name)
		{
			const std::string value = EnvironmentString(name);
			return value.empty() ? std::nullopt : std::optional<std::filesystem::path>(NormalPath(value));
		}

		bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs)
		{
			return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
		}

		bool MountAssetsPak(const std::vector<std::filesystem::path>& candidates)
		{
			std::optional<std::filesystem::path> selected;
			std::vector<std::filesystem::path> existing;
			for (const auto& candidate: candidates)
			{
				if (PathExists(candidate))
				{
					AddUniquePath(existing, candidate);
					if (!selected)
					{
						selected = NormalPath(candidate);
					}
				}
			}

			if (!selected)
			{
				return false;
			}

			for (const auto& pak: existing)
			{
				if (pak != *selected)
				{
					AE_WARN(LogCategory::FileSystem, "Ignoring alternate assets pak '{}' because '{}' was selected.", pak.string(), selected->string());
				}
			}

			FileSystem::MountPak("assets", *selected);
			return true;
		}

		void MountAssetsDirectory(std::filesystem::path directory)
		{
			directory = NormalPath(std::move(directory));
			AE_WARN(LogCategory::FileSystem, "Mounting loose asset directory. Processed assets such as .mesh/.texture may be unavailable unless this directory contains generated outputs: {}", directory.string());
			FileSystem::Mount("assets", std::move(directory));
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

		// -- assets:// ---------------------------------------------------------
		// Selection is intentionally deterministic:
		//   AETHER_ASSET_MODE=pak|dir|auto
		//   AETHER_ASSET_PAK=<pak path>   overrides pak candidates
		//   AETHER_ASSET_DIR=<directory>  overrides loose directory fallback
		// Default pak candidates are run-directory data/ first (ship layout),
		// then the CMake build data dir (dev layout).
		const std::string assetMode = EnvironmentString("AETHER_ASSET_MODE");
		if (EqualsIgnoreCase(assetMode, "dir"))
		{
			MountAssetsDirectory(EnvironmentPath("AETHER_ASSET_DIR")
			                .value_or(
#ifdef AETHER_DEFAULT_ASSET_DIR
			                        std::filesystem::path(AETHER_DEFAULT_ASSET_DIR)
#else
			                        workingDirectory / "assets"
#endif
			                                ));
		}
		else
		{
			std::vector<std::filesystem::path> pakCandidates;
			if (auto overridePak = EnvironmentPath("AETHER_ASSET_PAK"))
			{
				AddUniquePath(pakCandidates, *overridePak);
			}

			AddUniquePath(pakCandidates, workingDirectory / "data" / "assets.pak");
			AddUniquePath(pakCandidates, workingDirectory / "../data/assets.pak");
			AddUniquePath(pakCandidates, workingDirectory / "../../data/assets.pak");
#ifdef AETHER_DEFAULT_ASSET_PAK
			AddUniquePath(pakCandidates, AETHER_DEFAULT_ASSET_PAK);
#endif

			if (!MountAssetsPak(pakCandidates))
			{
				if (EqualsIgnoreCase(assetMode, "pak"))
				{
					AE_ASSERT_ALWAYS(false, "AETHER_ASSET_MODE=pak but no usable assets.pak was found. Set AETHER_ASSET_PAK or build App to generate data/assets.pak.");
				}

				MountAssetsDirectory(EnvironmentPath("AETHER_ASSET_DIR")
				                .value_or(
#ifdef AETHER_DEFAULT_ASSET_DIR
				                        std::filesystem::path(AETHER_DEFAULT_ASSET_DIR)
#else
				                        workingDirectory / "assets"
#endif
				                                ));
			}
		}

		// -- shaders:// --------------------------------------------------------
		const auto shaderDirectory = ResolveMountedDirectory({
		        workingDirectory / "shaders",
		        workingDirectory / "build/shaders",
		        workingDirectory / "../shaders",
		        workingDirectory / "../../shaders",
		        workingDirectory / "../build/shaders",
		        workingDirectory / "../../build/shaders",
		});
		AE_INFO(LogCategory::FileSystem, "CWD for shader mount: '{}' -> resolved: '{}'", workingDirectory.string(), shaderDirectory.string());
		Mount("shaders", shaderDirectory);

		// -- config:// ---------------------------------------------------------
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

		// -- data:// ------------------------------------------------------------
		// Game data files (JSON configs, NPC definitions, dialogues, etc.)
		const auto dataDirectory = ResolveMountedDirectory({
		        workingDirectory / "data/data",
		        workingDirectory / "../data/data",
		        workingDirectory / "../../data/data",
		});
		Mount("data", dataDirectory);

		// -- scripts:// ----------------------------------------------------------
		// Managed C# assemblies are deployed here (data/scripts/managed) by the
		// ManagedAssemblies build target.
		const auto scriptsDirectory = ResolveMountedDirectory({
		        workingDirectory / "data/scripts",
		        workingDirectory / "../data/scripts",
		        workingDirectory / "../../data/scripts",
		});
		Mount("scripts", scriptsDirectory);

		// -- logs:// -----------------------------------------------------------
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
		AE_PROFILE_ZONE();
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
		AE_PROFILE_ZONE();
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
		AE_PROFILE_ZONE();
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
			        try
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
			        }
			        catch (...)
			        {
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
		AE_PROFILE_ZONE();
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::ReadFileAsync() called before Initialize().");
		}

		auto pair = coro::task<std::vector<std::byte>>::create();
		const std::string pathStr(virtualPath);

		s_backend->ioThread->Submit(priority,
		        [pathStr, source = std::move(pair.second)]() mutable
		        {
			        try
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
			        }
			        catch (...)
			        {
				        source.set_exception(std::current_exception());
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
