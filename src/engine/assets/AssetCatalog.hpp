#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "utils/Expected.hpp"

namespace aether
{
	struct AssetMigrationReport
	{
		std::vector<std::string> changes;
		std::filesystem::path backupDirectory;
	};

	struct AssetReferenceLocation
	{
		std::string document;
		std::string field;
		std::string uid;
	};

	class AssetCatalog
	{
	public:
		static Expected<AssetCatalog> Load(const std::filesystem::path& projectRoot);
		static Expected<AssetCatalog> Parse(std::string_view text);
		static Expected<AssetMigrationReport> Migrate(const std::filesystem::path& projectRoot, bool apply);
		[[nodiscard]] std::string Serialize() const;
		Expected<void> Move(const std::filesystem::path& sourceRelative, const std::filesystem::path& targetRelative);
		[[nodiscard]] std::string Reference(std::string_view projectVfsPath) const;
		[[nodiscard]] std::string Resolve(std::string_view uidOrPath) const;
		[[nodiscard]] Expected<std::vector<AssetReferenceLocation>> References(std::string_view uidOrReference) const;

	private:
		std::filesystem::path m_root;
		std::filesystem::path m_scenesDirectory = "scenes";
		std::filesystem::path m_prefabsDirectory = "assets/prefabs";
		Expected<void> ReadProjectDirectories();
		std::unordered_map<std::string, std::string> m_uidToPath;
		std::unordered_map<std::string, std::string> m_pathToUid;
		std::vector<AssetReferenceLocation> m_references;
	};
} // namespace aether
