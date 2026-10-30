#include "WebFileSystem.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <optional>
#include <regex>
#include "io/GlobMatch.hpp"
#include "assets/AssetCatalog.hpp"
#include "io/FileSystem.hpp"
#include "io/MemoryPak.hpp"
#include "utils/Utf8.hpp"

namespace
{
	const aether::io::MemoryPak* s_package = nullptr;
	const aether::io::MemoryPak* s_engine = nullptr;
	bool s_initialized = false;
	std::uint64_t s_generation = 0;
	std::optional<aether::AssetCatalog> s_catalog;

	std::string_view Relative(std::string_view path)
	{
		return path.starts_with("project://") ? path.substr(10) : path.starts_with("engine://") ? path.substr(9) : std::string_view{};
	}
} // namespace

namespace aether::web
{
	void SetWebPackage(const io::MemoryPak* package, const io::MemoryPak* engine)
	{
		if (s_package != package || s_engine != engine)
		{
			s_package = package;
			s_engine = engine;
			++s_generation;
			s_catalog.reset();
			if (s_package)
				(void)io::FileSystem::ReloadAssetCatalog();
		}
	}
} // namespace aether::web

namespace aether::io
{
	void FileSystem::Initialize()
	{
		s_initialized = true;
	}

	void FileSystem::InitializeDefaultMounts()
	{
		Initialize();
	}

	void FileSystem::Shutdown()
	{
		web::SetWebPackage(nullptr);
		s_initialized = false;
	}

	bool FileSystem::IsInitialized()
	{
		return s_initialized;
	}

	bool FileSystem::IsMounted(std::string_view mountPoint)
	{
		return (s_package && mountPoint == "project") || (s_engine && mountPoint == "engine");
	}

	Expected<void> FileSystem::ReloadAssetCatalog()
	{
		std::optional<AssetCatalog> catalog;
		if (s_package && s_package->Exists("asset-catalog.toml"))
		{
			AE_TRY(bytes, s_package->Read("asset-catalog.toml"));
			AE_TRY(parsed, AssetCatalog::Parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size())));
			catalog = std::move(*parsed);
		}
		s_catalog = std::move(catalog);
		return {};
	}

	std::string FileSystem::ResolveAssetPath(std::string_view path)
	{
		if (!path.starts_with("uid://"))
			return std::string(path);
		return s_catalog ? s_catalog->Resolve(path) : std::string{};
	}

	std::string FileSystem::AssetReference(std::string_view path)
	{
		return s_catalog && path.starts_with("project://") ? s_catalog->Reference(path) : std::string(path);
	}

	bool FileSystem::Exists(std::string_view path)
	{
		const std::string resolved = ResolveAssetPath(path);
		path = resolved;
		const auto* package = path.starts_with("project://") ? s_package : path.starts_with("engine://") ? s_engine : nullptr;
		return package && package->Exists(Relative(path));
	}

	Expected<std::vector<std::byte>> FileSystem::ReadFile(std::string_view path)
	{
		const std::string resolved = ResolveAssetPath(path);
		path = resolved;
		const auto* package = path.starts_with("project://") ? s_package : path.starts_with("engine://") ? s_engine : nullptr;
		if (!s_initialized || !package)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Browser project package is not mounted: " + std::string(path)));
		}
		return package->Read(Relative(path));
	}

	Expected<std::string> FileSystem::ReadFileText(std::string_view path)
	{
		AE_TRY(bytes, ReadFile(path));
		if (bytes->size() > 4u * 1024u * 1024u || !IsValidUtf8(reinterpret_cast<const char*>(bytes->data()), bytes->size()))
		{
			AE_UNEXPECTED(AetherError::Asset("Browser text asset exceeds 4 MiB or is not UTF-8"));
		}
		return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
	}

	Expected<void> FileSystem::WriteFile(std::string_view, std::span<const std::byte>)
	{
		AE_UNEXPECTED(AetherError::FileSystem("Browser packages are read-only; use gameplay save storage"));
	}

	Expected<void> FileSystem::WriteFileText(std::string_view path, std::string_view text)
	{
		return WriteFile(path, {reinterpret_cast<const std::byte*>(text.data()), text.size()});
	}

	Expected<std::unique_ptr<std::istream>> FileSystem::OpenStream(std::string_view path)
	{
		AE_TRY(bytes, ReadFile(path));
		return std::unique_ptr<std::istream>(std::make_unique<std::istringstream>(
		        std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size())));
	}

	Expected<std::vector<std::string>> FileSystem::Glob(std::string_view pattern, const FileGlobOptions& options)
	{
		const auto separator = pattern.find("://");
		if (separator == std::string_view::npos)
		{
			AE_UNEXPECTED(AetherError::FileSystem("Invalid virtual path (missing ://): " + std::string(pattern)));
		}
		const auto mount = pattern.substr(0, separator);
		const auto* package = mount == "project" ? s_package : mount == "engine" ? s_engine : nullptr;
		if (!s_initialized || !package)
		{
			return std::vector<std::string>{};
		}
		const auto relative = pattern.substr(separator + 3);
		if (relative.size() > 4096 || relative.find('\0') != std::string_view::npos || !IsValidUtf8(relative.data(), relative.size()))
		{
			AE_UNEXPECTED(AetherError::FileSystem("Invalid browser package glob pattern"));
		}
		const auto flags = options.caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase;
		const std::regex matcher(GlobToRegex(relative), flags);
		auto paths = package->Paths();
		std::sort(paths.begin(), paths.end());
		paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
		std::erase_if(paths, [&matcher](const std::string& path) { return !std::regex_search(path, matcher); });
		return paths;
	}

	std::uint64_t FileSystem::ContentStamp(std::string_view path)
	{
		return Exists(path) ? s_generation : 0;
	}

	std::uint64_t FileSystem::ShaderOverlayGeneration()
	{
		return 0;
	}

	void FileSystem::Flush()
	{
	}

	std::string FileSystem::ResolveRelative(std::string_view base, std::string_view relative)
	{
		if (relative.find("://") != std::string_view::npos)
			return ResolveAssetPath(relative);
		const std::string resolvedBase = ResolveAssetPath(base);
		if (base.starts_with("uid://") && resolvedBase.empty())
			return {};
		base = resolvedBase;
		if (base.find("://") == std::string_view::npos)
			return std::string(relative);
		const auto scheme = base.find("://") + 3;
		const std::filesystem::path path(base.substr(scheme));
		return std::string(base.substr(0, scheme)) + (path.parent_path() / relative).lexically_normal().generic_string();
	}
} // namespace aether::io
