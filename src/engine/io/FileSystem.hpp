#pragma once

#include <expected>
#include <filesystem>
#include <istream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "FileGlobOptions.hpp"
#include "IOPriority.hpp"
#include "OverlayBackend.hpp"
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

		static void Mount(std::string_view mountPoint, std::filesystem::path physicalPath);

		static void MountPak(std::string_view mountPoint, std::filesystem::path pakPath);

		static void MountShaderOverlay(std::optional<OverlayBackend::Layer> projectLayer);

		// Bumped every time the shader overlay is (re)mounted (e.g. after a project shader
		// recompile). Consumers that cache compiled-shader pipelines poll this to know when to
		// rebuild them from the fresh .spv. Thread-safe.
		[[nodiscard]] static std::uint64_t ShaderOverlayGeneration();

		[[nodiscard]] static bool Exists(std::string_view virtualPath);

		[[nodiscard]] static Expected<std::vector<std::byte>> ReadFile(std::string_view virtualPath);

		[[nodiscard]] static Expected<std::string> ReadFileText(std::string_view virtualPath);

		[[nodiscard]] static Expected<void> WriteFile(std::string_view virtualPath, std::span<const std::byte> data);

		[[nodiscard]] static Expected<void> WriteFileText(std::string_view virtualPath, std::string_view text);

		[[nodiscard]] static Expected<std::unique_ptr<std::istream>> OpenStream(std::string_view virtualPath);

		[[nodiscard]] static Expected<std::vector<std::string>> Glob(std::string_view virtualPattern, const FileGlobOptions& options = {});

		// file has been read on the background I/O thread.  The calling
		[[nodiscard]] static coro::task<std::vector<std::byte>> ReadFileAsync(std::string_view virtualPath, IOPriority priority = IOPriority::Normal);

		static void Flush();
	};
} // namespace aether::io
