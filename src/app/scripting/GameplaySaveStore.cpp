#include "GameplaySaveStore.hpp"

#include <algorithm>
#include <utility>

#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"

namespace aether::app::scripting
{
	namespace
	{
		bool SafeKey(std::string_view key)
		{
			return !key.empty() && key.size() <= 128 && std::all_of(key.begin(), key.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
		}
	}

	GameplaySaveStore::GameplaySaveStore(std::filesystem::path directory) : m_directory(std::move(directory)) {}

	std::filesystem::path GameplaySaveStore::ProjectDirectory()
	{
		const auto root = io::PlatformPaths::GetUserConfigDir();
		if (root.empty())
		{
			return {};
		}
		const auto project = io::PlatformPaths::ReadEnvironmentPath("AETHER_PROJECT_DIR");
		auto source = !project.empty() ? project.lexically_normal() : io::PlatformPaths::GetExecutableDir();
		if (source.filename().empty())
		{
			source = source.parent_path();
		}
		constexpr char kHex[] = "0123456789abcdef";
		std::string nameSpace = "project-";
		for (unsigned char byte: source.filename().u8string())
		{
			nameSpace += kHex[byte >> 4];
			nameSpace += kHex[byte & 15];
		}
		// Keep the original Rust namespace so existing player saves remain readable.
		return root / "rust-saves" / nameSpace;
	}

	AetherGameplayStatus GameplaySaveStore::Read(std::string_view key, std::string& output, bool allowReads) const
	{
		if (m_directory.empty()) { return AE_GAMEPLAY_UNSUPPORTED; }
		if (!SafeKey(key)) { return AE_GAMEPLAY_INVALID_ARGUMENT; }
		if (!allowReads) { return AE_GAMEPLAY_INVALID_HANDLE; }
		const auto saved = io::file_util::ReadText(m_directory / (std::string(key) + ".txt"));
		if (!saved) { return AE_GAMEPLAY_INVALID_HANDLE; }
		output = *saved;
		return AE_GAMEPLAY_OK;
	}

	AetherGameplayStatus GameplaySaveStore::Write(std::string_view key, std::string_view value) const
	{
		if (m_directory.empty()) { return AE_GAMEPLAY_UNSUPPORTED; }
		if (!SafeKey(key)) { return AE_GAMEPLAY_INVALID_ARGUMENT; }
		if (!io::file_util::CreateDirectories(m_directory)) { return AE_GAMEPLAY_SCRIPT_ERROR; }
		return io::file_util::WriteTextAtomic(m_directory / (std::string(key) + ".txt"), value) ? AE_GAMEPLAY_OK : AE_GAMEPLAY_SCRIPT_ERROR;
	}
}
