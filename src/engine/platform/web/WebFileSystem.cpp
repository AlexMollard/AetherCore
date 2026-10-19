#include "WebFileSystem.hpp"

#include <filesystem>
#include <sstream>
#include "io/FileSystem.hpp"
#include "io/MemoryPak.hpp"
#include "utils/Utf8.hpp"

namespace
{
	const aether::io::MemoryPak* s_package = nullptr;
	const aether::io::MemoryPak* s_engine = nullptr;
	bool s_initialized = false;
	std::uint64_t s_generation = 0;

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

	bool FileSystem::Exists(std::string_view path)
	{
		const auto* package = path.starts_with("project://") ? s_package : path.starts_with("engine://") ? s_engine : nullptr;
		return package && package->Exists(Relative(path));
	}

	Expected<std::vector<std::byte>> FileSystem::ReadFile(std::string_view path)
	{
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
		AE_TRY(text, ReadFileText(path));
		return std::unique_ptr<std::istream>(std::make_unique<std::istringstream>(std::move(*text)));
	}

	Expected<std::vector<std::string>> FileSystem::Glob(std::string_view, const FileGlobOptions&)
	{
		AE_UNEXPECTED(AetherError::FileSystem("Browser package globbing is not supported in this profile"));
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
		if (relative.find("://") != std::string_view::npos || base.find("://") == std::string_view::npos)
		{
			return std::string(relative);
		}
		const auto scheme = base.find("://") + 3;
		const std::filesystem::path path(base.substr(scheme));
		return std::string(base.substr(0, scheme)) + (path.parent_path() / relative).lexically_normal().generic_string();
	}
} // namespace aether::io
