#include "FileUtil.hpp"

#include <ctime>
#include <cstdlib>
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
	bool HasTrashSupport()
	{
#if defined(_WIN32)
		return true;
#else
		// Same discovery MoveToTrash does: with neither variable there is nowhere to put it.
		const char* xdg = std::getenv("XDG_DATA_HOME");
		const char* home = std::getenv("HOME");
		return (xdg != nullptr && xdg[0] != 0) || (home != nullptr && home[0] != 0);
#endif
	}

	std::string_view TrashDisplayName()
	{
#if defined(_WIN32)
		return "Recycle Bin";
#else
		return "Trash";
#endif
	}

	std::string BuildTrashInfo(const std::filesystem::path& originalPath, const std::string_view deletionDateIso)
	{
		// Path is percent-encoded per RFC 2396, except '/' which stays a separator. Without
		// this a path containing a space or a '#' produces an info file the desktop cannot
		// parse, and the trashed file becomes unrestorable.
		const std::string absolute = originalPath.generic_string();
		std::string encoded;
		encoded.reserve(absolute.size());
		for (const unsigned char c: absolute)
		{
			const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			        || c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
			if (unreserved)
			{
				encoded.push_back(static_cast<char>(c));
				continue;
			}
			static constexpr char kHex[] = "0123456789ABCDEF";
			encoded.push_back('%');
			encoded.push_back(kHex[c >> 4]);
			encoded.push_back(kHex[c & 0x0F]);
		}

		std::string info = "[Trash Info]\n";
		info += "Path=" + encoded + "\n";
		info += "DeletionDate=" + std::string(deletionDateIso) + "\n";
		return info;
	}

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
		// freedesktop.org trash spec: move the file under $XDG_DATA_HOME/Trash/files and drop
		// a matching .trashinfo beside it so the desktop can offer "restore".
		//
		// Every failure returns false WITHOUT having moved anything, so the caller falls back
		// to the permanent delete it would have done anyway. The one thing this must never do
		// is report success while leaving the file somewhere nothing can restore it from.
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
		{
			return false;
		}

		std::filesystem::path dataHome;
		if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != 0)
		{
			dataHome = xdg;
		}
		else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != 0)
		{
			dataHome = std::filesystem::path(home) / ".local" / "share";
		}
		else
		{
			return false;
		}

		const std::filesystem::path trashDir = dataHome / "Trash";
		const std::filesystem::path filesDir = trashDir / "files";
		const std::filesystem::path infoDir = trashDir / "info";
		std::filesystem::create_directories(filesDir, ec);
		std::filesystem::create_directories(infoDir, ec);
		if (ec)
		{
			return false;
		}

		const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
		if (ec)
		{
			return false;
		}

		// The spec requires the names in files/ and info/ to match and to be unique. Two
		// deletes of the same filename would otherwise overwrite each other's entry.
		std::filesystem::path target = filesDir / absolute.filename();
		std::filesystem::path infoFile = infoDir / (absolute.filename().string() + ".trashinfo");
		for (int suffix = 1; (std::filesystem::exists(target, ec) || std::filesystem::exists(infoFile, ec)) && suffix < 10000; ++suffix)
		{
			const std::string stem = absolute.stem().string() + "." + std::to_string(suffix);
			const std::string name = stem + absolute.extension().string();
			target = filesDir / name;
			infoFile = infoDir / (name + ".trashinfo");
		}
		if (std::filesystem::exists(target, ec))
		{
			return false;
		}

		// The info file goes down FIRST: a file in files/ with no info beside it is an orphan
		// the desktop cannot restore, whereas a stale info file with no data is harmless.
		const std::time_t now = std::time(nullptr);
		std::tm local{};
#	if defined(_WIN32)
		localtime_s(&local, &now);
#	else
		localtime_r(&now, &local);
#	endif
		char stamp[32]{};
		(void) std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
		if (auto written = WriteText(infoFile, BuildTrashInfo(absolute, stamp)); !written)
		{
			return false;
		}

		// rename() only works within a filesystem. Across one, fall back rather than copying:
		// a half-copied directory tree is worse than the permanent delete the caller will do.
		std::filesystem::rename(absolute, target, ec);
		if (ec)
		{
			std::error_code cleanup;
			std::filesystem::remove(infoFile, cleanup);
			return false;
		}
		return true;
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
