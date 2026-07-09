#include "FileUtil.hpp"

#include <fstream>
#include <sstream>

#include "utils/Expected.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

namespace aether::io::file_util
{
	Expected<std::string> ReadText(const std::filesystem::path& path)
	{
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
		{
			return std::unexpected(AetherError::FileSystem(std::format("File not found: {}", path.string())));
		}

		std::ifstream in(path, std::ios::binary);
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

		std::ofstream out(path, std::ios::trunc);
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
		if (!in.read(reinterpret_cast<char*>(data.data()), data.size()))
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
