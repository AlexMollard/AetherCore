#include "FileUtil.hpp"

#include <fstream>
#include <sstream>

#include "utils/Expected.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	include <windows.h>

#	include <shellapi.h>

// windows.h defines CopyFile as a macro that expands to CopyFileA/W, which silently renames
// this file's own file_util::CopyFile and leaves every caller unresolved at link time.
#	undef CopyFile
#endif

namespace aether::io::file_util
{
	bool MoveToTrash(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
		{
			return false;
		}
		// SHFileOperationW wants the source double-null terminated: it takes a LIST of paths
		// and reads until an empty one. A single-terminated string walks off the end.
		std::wstring source = std::filesystem::absolute(path, ec).wstring();
		source.push_back(L'\0');
		source.push_back(L'\0');

		SHFILEOPSTRUCTW op{};
		op.wFunc = FO_DELETE;
		op.pFrom = source.c_str();
		// ALLOWUNDO is what makes it the recycle bin rather than a delete. NOCONFIRMATION
		// because the editor has already asked, and NOERRORUI so a failure comes back as a
		// return code instead of a dialog behind the editor window.
		op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
		return SHFileOperationW(&op) == 0 && op.fAnyOperationsAborted == FALSE;
#else
		// No trash implementation on this platform yet; the caller falls back to a permanent
		// delete rather than this silently doing one.
		(void) path;
		return false;
#endif
	}

	Expected<std::string> ReadText(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
		{
			return std::unexpected(AetherError::FileSystem(std::format("File not found: {}", path.string())));
		}

		const std::ifstream in(path, std::ios::binary);
		if (!in.is_open())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to open file for reading: {}", path.string())));
		}

		std::ostringstream buffer;
		buffer << in.rdbuf();

		if (in.bad())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to read file: {}", path.string())));
		}

		return buffer.str();
	}

	Expected<void> WriteText(const std::filesystem::path& path, std::string_view text)
	{
		std::error_code ec;

		auto parent = path.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				return std::unexpected(AetherError::FileSystem(std::format("Failed to create directories for '{}': {}", path.string(), ec.message())));
			}
		}

		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to open file for writing: {}", path.string())));
		}

		out << text;
		out.close();

		if (out.fail())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to write file: {}", path.string())));
		}

		return {};
	}

	Expected<std::vector<std::byte>> ReadBinary(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
		{
			return std::unexpected(AetherError::FileSystem(std::format("File not found: {}", path.string())));
		}

		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in.is_open())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to open file for reading: {}", path.string())));
		}

		const auto size = in.tellg();
		if (size < 0)
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to determine file size: {}", path.string())));
		}

		in.seekg(0, std::ios::beg);

		std::vector<std::byte> data(static_cast<std::size_t>(size));
		if (!in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to read file: {}", path.string())));
		}

		return data;
	}

	Expected<void> WriteBinary(const std::filesystem::path& path, std::span<const std::byte> data)
	{
		std::error_code ec;

		auto parent = path.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				return std::unexpected(AetherError::FileSystem(std::format("Failed to create directories for '{}': {}", path.string(), ec.message())));
			}
		}

		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out.is_open())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to open file for writing: {}", path.string())));
		}

		out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		out.close();

		if (out.fail())
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to write file: {}", path.string())));
		}

		return {};
	}

	Expected<void> CreateDirectories(const std::filesystem::path& path)
	{
		std::error_code ec;
		std::filesystem::create_directories(path, ec);
		if (ec)
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to create directories '{}': {}", path.string(), ec.message())));
		}
		return {};
	}

	Expected<void> CopyFile(const std::filesystem::path& from, const std::filesystem::path& to, bool overwrite)
	{
		std::error_code ec;

		auto parent = to.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				return std::unexpected(AetherError::FileSystem(std::format("Failed to create destination directories: {}", ec.message())));
			}
		}

		auto options = overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::none;

		std::filesystem::copy_file(from, to, options, ec);
		if (ec)
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to copy '{}' to '{}': {}", from.string(), to.string(), ec.message())));
		}
		return {};
	}

	bool Exists(const std::filesystem::path& path)
	{
		std::error_code ec;
		return std::filesystem::exists(path, ec);
	}

	Expected<void> Remove(const std::filesystem::path& path)
	{
		std::error_code ec;
		std::filesystem::remove(path, ec);
		if (ec)
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to remove '{}': {}", path.string(), ec.message())));
		}
		return {};
	}

	Expected<uintmax_t> FileSize(const std::filesystem::path& path)
	{
		std::error_code ec;
		auto size = std::filesystem::file_size(path, ec);
		if (ec)
		{
			return std::unexpected(AetherError::FileSystem(std::format("Failed to get file size for '{}': {}", path.string(), ec.message())));
		}
		return size;
	}
} // namespace aether::io::file_util
