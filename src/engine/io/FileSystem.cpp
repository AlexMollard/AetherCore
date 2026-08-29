#include "FileSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>
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
#include "IOThread.hpp"
#include "PlatformPaths.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"
#include "OverlayBackend.hpp"
#include "PakBackend.hpp"

namespace aether::io
{
	namespace
	{
		struct FileSystemBackend
		{
			std::map<std::string, std::shared_ptr<IFileBackend>, std::less<>> mounts;
			std::mutex mountsMutex;
			std::unique_ptr<IoExecutor> ioThread;

			// does so before any other thread can be touching s_backend. Every
			std::optional<OverlayBackend::Layer> engineShaderLayer;
		};

		FileSystemBackend* s_backend = nullptr;

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

		// Null when nothing is mounted there. Reading through a mount point that does not
		// exist is a MISS, not a programmer error: callers legitimately probe several
		// prefixes and take the first that answers - FontRegistry tries project:// before
		// engine:// so a game can override a typeface. Asserting instead made every such
		// probe fatal in any app that mounts no project, which is exactly what the
		// Launcher is. It only ever survived because a developer build mounts the sample
		// project baked in at configure time, so the crash could not happen on the machine
		// that built it.
		std::shared_ptr<IFileBackend> TryResolveBackend(std::string_view mountPoint)
		{
			const std::scoped_lock lock(s_backend->mountsMutex);
			auto it = s_backend->mounts.find(mountPoint);
			return it == s_backend->mounts.end() ? nullptr : it->second;
		}

		// For the mount wiring below, where the mount has just been established and its
		// absence really would be a bug in this file.
		std::shared_ptr<IFileBackend> ResolveBackend(std::string_view mountPoint)
		{
			std::shared_ptr<IFileBackend> backend = TryResolveBackend(mountPoint);
			if (backend == nullptr)
			{
				AE_ASSERT_ALWAYS(false, "No backend mounted at: " + std::string(mountPoint));
			}
			return backend;
		}

		AetherError NotMounted(std::string_view mountPoint, std::string_view virtualPath)
		{
			return AetherError::FileSystem(std::format("nothing is mounted at '{}://' (reading '{}')", mountPoint, virtualPath));
		}

		void MountBackend(std::string_view mountPoint, std::shared_ptr<IFileBackend> backend)
		{
			if (s_backend == nullptr)
			{
				AE_ASSERT_ALWAYS(false, "MountBackend() called before Initialize().");
			}

			AE_INFO(LogCategory::FileSystem, "Mounting '{}://' -> <composite backend>", mountPoint);
			const std::scoped_lock lock(s_backend->mountsMutex);
			s_backend->mounts.insert_or_assign(std::string(mountPoint), std::move(backend));
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
			return PlatformPaths::ReadEnvironmentVariable(name);
		}

		std::optional<std::filesystem::path> EnvironmentPath(const char* name)
		{
			const std::string value = EnvironmentString(name);
			return value.empty() ? std::nullopt : std::optional<std::filesystem::path>(NormalPath(value));
		}

		std::string EnvironmentStringFirst(const char* primary, const char* legacy)
		{
			std::string value = EnvironmentString(primary);
			if (!value.empty())
			{
				return value;
			}
			return EnvironmentString(legacy);
		}

		std::optional<std::filesystem::path> EnvironmentPathFirst(const char* primary, const char* legacy)
		{
			if (auto value = EnvironmentPath(primary))
			{
				return value;
			}
			return EnvironmentPath(legacy);
		}

		bool MountPakCandidate(std::string_view mountPoint, std::string_view pakName, const std::vector<std::filesystem::path>& candidates)
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

			// Verbose, not a warning: the candidate list deliberately spans the ship layout and
			// the dev build tree, so on a developer machine several of them exist and the first
			// one legitimately wins. Warning made a normal situation look like a fault - and it
			// fires from a published package too, naming the build tree it came from. The
			// "Mounting pak" line below already records which one was chosen.
			for (const auto& pak: existing)
			{
				if (pak != *selected)
				{
					AE_VERBOSE(LogCategory::FileSystem, "Ignoring alternate {} pak '{}' because '{}' was selected.", pakName, pak.string(), selected->string());
				}
			}

			FileSystem::MountPak(mountPoint, *selected);
			return true;
		}

		bool MountEnginePak(const std::vector<std::filesystem::path>& candidates)
		{
			return MountPakCandidate("engine", "engine", candidates);
		}

		bool MountProjectPak(const std::vector<std::filesystem::path>& candidates)
		{
			return MountPakCandidate("project", "project", candidates);
		}

		void MountEngineDirectory(std::filesystem::path directory)
		{
			directory = NormalPath(std::move(directory));
			AE_WARN(LogCategory::FileSystem, "Mounting loose engine resource directory. Built-in resources such as fonts may be unavailable unless this directory contains generated outputs: {}", directory.string());
			FileSystem::Mount("engine", std::move(directory));
		}

		void MountProjectDirectory(std::filesystem::path directory)
		{
			if (directory.empty())
			{
				return;
			}

			directory = NormalPath(std::move(directory));
			if (!PathExists(directory))
			{
				return;
			}

			AE_INFO(LogCategory::FileSystem, "Mounting loose project directory: {}", directory.string());
			FileSystem::Mount("project", std::move(directory));
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

	bool FileSystem::IsMounted(std::string_view mountPoint)
	{
		if (s_backend == nullptr)
		{
			return false;
		}
		const std::scoped_lock lock(s_backend->mountsMutex);
		return s_backend->mounts.contains(std::string(mountPoint));
	}

	void FileSystem::InitializeDefaultMounts()
	{
		Initialize();

		const auto workingDirectory = std::filesystem::current_path();

		// Default pak candidates are run-directory data/ first (ship layout),
		bool engineUsesPak = false;
		const std::string engineMode = EnvironmentStringFirst("AETHER_ENGINE_MODE", "AETHER_ASSET_MODE");
		if (utils::IEq(engineMode, "dir"))
		{
			MountEngineDirectory(EnvironmentPathFirst("AETHER_ENGINE_DIR", "AETHER_ASSET_DIR")
			                .value_or(
#ifdef AETHER_DEFAULT_ENGINE_DIR
			                        std::filesystem::path(AETHER_DEFAULT_ENGINE_DIR)
#else
			                        workingDirectory / "engine"
#endif
			                                ));
		}
		else
		{
			std::vector<std::filesystem::path> pakCandidates;
			if (auto overridePak = EnvironmentPathFirst("AETHER_ENGINE_PAK", "AETHER_ASSET_PAK"))
			{
				AddUniquePath(pakCandidates, *overridePak);
			}

			AddUniquePath(pakCandidates, workingDirectory / "data" / "engine.pak");
			AddUniquePath(pakCandidates, workingDirectory / "../data/engine.pak");
			AddUniquePath(pakCandidates, workingDirectory / "../../data/engine.pak");
#ifdef AETHER_DEFAULT_ENGINE_PAK
			AddUniquePath(pakCandidates, AETHER_DEFAULT_ENGINE_PAK);
#endif

			if (MountEnginePak(pakCandidates))
			{
				engineUsesPak = true;
			}
			else
			{
				if (utils::IEq(engineMode, "pak"))
				{
					AE_ASSERT_ALWAYS(false, "AETHER_ENGINE_MODE=pak but no usable engine.pak was found. Set AETHER_ENGINE_PAK or build the Editor to generate data/engine.pak.");
				}

				MountEngineDirectory(EnvironmentPathFirst("AETHER_ENGINE_DIR", "AETHER_ASSET_DIR")
				                .value_or(
#ifdef AETHER_DEFAULT_ENGINE_DIR
				                        std::filesystem::path(AETHER_DEFAULT_ENGINE_DIR)
#else
				                        workingDirectory / "engine"
#endif
				                                ));
			}
		}

		bool projectUsesPak = false;
		const std::string projectMode = EnvironmentString("AETHER_PROJECT_MODE");
		if (utils::IEq(projectMode, "dir"))
		{
			MountProjectDirectory(EnvironmentPath("AETHER_PROJECT_DIR")
			                .value_or(
#ifdef AETHER_DEFAULT_PROJECT_DIR
			                        std::filesystem::path(AETHER_DEFAULT_PROJECT_DIR)
#else
			                        std::filesystem::path{}
#endif
			                                ));
		}
		else
		{
			std::vector<std::filesystem::path> projectPakCandidates;
			if (auto overridePak = EnvironmentPath("AETHER_PROJECT_PAK"))
			{
				AddUniquePath(projectPakCandidates, *overridePak);
			}

			AddUniquePath(projectPakCandidates, workingDirectory / "data" / "project.pak");
			AddUniquePath(projectPakCandidates, workingDirectory / "../data/project.pak");
			AddUniquePath(projectPakCandidates, workingDirectory / "../../data/project.pak");
#ifdef AETHER_DEFAULT_PROJECT_PAK
			AddUniquePath(projectPakCandidates, AETHER_DEFAULT_PROJECT_PAK);
#endif

			projectUsesPak = MountProjectPak(projectPakCandidates);
			if (!projectUsesPak)
			{
				if (utils::IEq(projectMode, "pak"))
				{
					AE_ASSERT_ALWAYS(false, "AETHER_PROJECT_MODE=pak but no usable project.pak was found. Set AETHER_PROJECT_PAK or build the Editor to generate data/project.pak.");
				}

				MountProjectDirectory(EnvironmentPath("AETHER_PROJECT_DIR")
				                .value_or(
#ifdef AETHER_DEFAULT_PROJECT_DIR
				                        std::filesystem::path(AETHER_DEFAULT_PROJECT_DIR)
#else
				                        std::filesystem::path{}
#endif
				                                ));
			}
		}

		// that only ships engine.pak must still be able to resolve shaders://,
		OverlayBackend::Layer engineShaderLayer;
		if (engineUsesPak)
		{
			AE_INFO(LogCategory::FileSystem, "Mounting shaders:// from engine.pak ('shaders/' prefix)");
			engineShaderLayer = OverlayBackend::Layer{ResolveBackend("engine"), "shaders/"};
		}
		else
		{
			const auto shaderDirectory = ResolveMountedDirectory({
			        workingDirectory / "shaders",
			        workingDirectory / "build/shaders",
			        workingDirectory / "../shaders",
			        workingDirectory / "../../shaders",
			        workingDirectory / "../build/shaders",
			        workingDirectory / "../../build/shaders",
			});
			AE_INFO(LogCategory::FileSystem, "CWD for shader mount: '{}' -> resolved: '{}'", workingDirectory.string(), shaderDirectory.string());
			engineShaderLayer = OverlayBackend::Layer{std::make_shared<DirectoryBackend>(shaderDirectory), ""};
		}
		s_backend->engineShaderLayer = engineShaderLayer;

		// (GameRuntime never calls MountShaderOverlay itself - see
		if (projectUsesPak)
		{
			AE_INFO(LogCategory::FileSystem, "Mounting shaders:// with the project.pak layer prepended ('shaders/' prefix)");
			MountShaderOverlay(OverlayBackend::Layer{ResolveBackend("project"), "shaders/"});
		}
		else
		{
			MountShaderOverlay(std::nullopt);
		}

		const auto configDirectory = ResolveMountedDirectory({
		        workingDirectory / "data/config",
		        workingDirectory / "../data/config",
		        workingDirectory / "../../data/config",
		        workingDirectory / "config",
		        workingDirectory / "../config",
		        workingDirectory / "../../config",
		});
		Mount("config", configDirectory);

		const auto dataDirectory = ResolveMountedDirectory({
		        workingDirectory / "data/data",
		        workingDirectory / "../data/data",
		        workingDirectory / "../../data/data",
		});
		Mount("data", dataDirectory);

		const auto scriptsDirectory = ResolveMountedDirectory({
		        workingDirectory / "data/scripts",
		        workingDirectory / "../data/scripts",
		        workingDirectory / "../../data/scripts",
		});
		Mount("scripts", scriptsDirectory);

		Mount("logs", workingDirectory / "logs");
	}

	void FileSystem::Shutdown()
	{
		if (s_backend == nullptr)
		{
			return;
		}

		s_backend->ioThread->Flush();
		s_backend->ioThread.reset();
		{
			const std::scoped_lock lock(s_backend->mountsMutex);
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
		const std::scoped_lock lock(s_backend->mountsMutex);
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
		const std::scoped_lock lock(s_backend->mountsMutex);
		s_backend->mounts.insert_or_assign(std::string(mountPoint), std::make_shared<PakBackend>(std::move(pakPath)));
	}

	namespace
	{
		std::atomic<std::uint64_t> s_shaderOverlayGen{0};
	}

	void FileSystem::MountShaderOverlay(std::optional<OverlayBackend::Layer> projectLayer)
	{
		AE_PROFILE_ZONE();
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::MountShaderOverlay() called before Initialize().");
		}
		if (!s_backend->engineShaderLayer)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::MountShaderOverlay() called before InitializeDefaultMounts() built the engine shader layer.");
		}

		std::vector<OverlayBackend::Layer> shaderLayers;
		if (projectLayer)
		{
			AE_INFO(LogCategory::FileSystem, "Mounting shaders:// with a project layer prepended (highest priority)");
			shaderLayers.push_back(*std::move(projectLayer));
		}
		shaderLayers.push_back(*s_backend->engineShaderLayer);
		MountBackend("shaders", std::make_shared<OverlayBackend>(std::move(shaderLayers)));
		s_shaderOverlayGen.fetch_add(1, std::memory_order_relaxed);
	}

	std::uint64_t FileSystem::ShaderOverlayGeneration()
	{
		return s_shaderOverlayGen.load(std::memory_order_relaxed);
	}

	bool FileSystem::Exists(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::Exists() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		const auto backend = TryResolveBackend(mountPoint);
		return backend != nullptr && backend->Exists(relativePath);
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
		const auto backend = TryResolveBackend(mountPoint);
		if (backend == nullptr)
		{
			AE_UNEXPECTED(NotMounted(mountPoint, virtualPath));
		}
		return backend->Read(relativePath);
	}

	Expected<std::string> FileSystem::ReadFileText(std::string_view virtualPath)
	{
		AE_TRY(data, ReadFile(virtualPath));
		return std::string(reinterpret_cast<const char*>(data->data()), data->size());
	}

	Expected<void> FileSystem::WriteFile(std::string_view virtualPath, std::span<const std::byte> data)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::WriteFile() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		AE_VERBOSE(LogCategory::FileSystem, "WriteFile: {}", virtualPath);
		const auto backend = TryResolveBackend(mountPoint);
		if (backend == nullptr)
		{
			AE_UNEXPECTED(NotMounted(mountPoint, virtualPath));
		}
		return backend->Write(relativePath, data);
	}

	Expected<void> FileSystem::WriteFileText(std::string_view virtualPath, std::string_view text)
	{
		const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(text.data()), text.size());
		return WriteFile(virtualPath, data);
	}

	Expected<std::unique_ptr<std::istream>> FileSystem::OpenStream(std::string_view virtualPath)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::OpenStream() called before Initialize().");
		}

		const auto [mountPoint, relativePath] = ParseVirtualPath(virtualPath);
		AE_VERBOSE(LogCategory::FileSystem, "OpenStream: {}", virtualPath);
		const auto backend = TryResolveBackend(mountPoint);
		if (backend == nullptr)
		{
			AE_UNEXPECTED(NotMounted(mountPoint, virtualPath));
		}
		return backend->OpenStream(relativePath);
	}

	Expected<std::vector<std::string>> FileSystem::Glob(std::string_view virtualPattern, const FileGlobOptions& options)
	{
		if (s_backend == nullptr)
		{
			AE_ASSERT_ALWAYS(false, "FileSystem::Glob() called before Initialize().");
		}

		const auto [mountPoint, relativePattern] = ParseVirtualPath(virtualPattern);
		const auto backend = TryResolveBackend(mountPoint);
		if (backend == nullptr)
		{
			// An unmounted prefix contributes no matches, the same as an empty directory.
			return std::vector<std::string>{};
		}
		auto result = backend->Glob(relativePattern, options);
		if (!result.has_value())
		{
			return result;
		}
		AE_VERBOSE(LogCategory::FileSystem, "Glob: '{}' returned {} result(s)", virtualPattern, result->size());
		return result;
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
