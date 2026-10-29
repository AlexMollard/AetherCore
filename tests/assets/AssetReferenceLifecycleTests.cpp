#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "assets/AssetCatalog.hpp"
#include "assets/TileMapAsset.hpp"
#include "AssetPipeline.hpp"
#include "io/FileSystem.hpp"
#include "scene/SceneSerializer.hpp"

using namespace aether;
using namespace aether::app::scene;

TEST_CASE("Scene and prefab links follow GUIDs outside their original directories")
{
	const auto root = std::filesystem::temp_directory_path() / ("ae-prefab-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

	struct Cleanup
	{
		std::filesystem::path root;

		~Cleanup()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup{root};

	std::filesystem::create_directories(root / "assets/prefabs");
	std::filesystem::create_directories(root / "scenes");
	std::filesystem::create_directories(root / "assets/relocated");
	std::ofstream(root / "assets/prefabs/Hero.prefab.toml") << "[[entities]]\nname = 'Hero'\n";
	std::ofstream(root / "scenes/Other.scene.toml") << "[scene]\nversion=16\nname='Other'\n[[entities]]\nname='OtherEntity'\n";
	std::ofstream(root / "scenes/Start.scene.toml") << "[scene]\nversion=16\nname='Start'\n[[includes]]\nscene='Other'\n[[prefab_instances]]\nprefab='Hero'\n";
	REQUIRE(AssetCatalog::Migrate(root, true).has_value());
	auto catalog = AssetCatalog::Load(root);
	REQUIRE(catalog.has_value());
	const auto prefab = catalog->Reference("project://assets/prefabs/Hero.prefab.toml");
	const auto other = catalog->Reference("project://scenes/Other.scene.toml");
	REQUIRE(catalog->Move("assets/prefabs/Hero.prefab.toml", "assets/relocated/Renamed.prefab.toml").has_value());
	REQUIRE(catalog->Move("scenes/Other.scene.toml", "assets/relocated/Other.scene.toml").has_value());
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("project", root);
	REQUIRE(io::FileSystem::ReloadAssetCatalog().has_value());
	const auto source = io::FileSystem::ReadFileText("project://scenes/Start.scene.toml");
	REQUIRE(source.has_value());
	const auto start = ParseToml(*source);
	REQUIRE(start.has_value());
	REQUIRE(start->prefabInstances.size() == 1);
	REQUIRE(start->includes.size() == 1);
	const auto hero = ReadPrefabFile(start->prefabInstances[0].prefabPath);
	REQUIRE(hero.has_value());
	REQUIRE(hero->entities.size() == 1);
	CHECK(hero->entities[0].name == "Hero");
	const auto included = ReadSceneFile(start->includes[0]);
	REQUIRE(included.has_value());
	CHECK(included->name == "Other");
	CHECK_FALSE(ReadPrefabFile(other).has_value());
	CHECK_FALSE(ReadSceneFile(prefab).has_value());
	std::ofstream(root / "assets/relocated/Renamed.prefab.toml", std::ios::trunc) << "[[entities]]\nname='EditedHero'\n";
	const auto edited = ReadPrefabFile(prefab);
	REQUIRE(edited.has_value());
	REQUIRE(edited->entities.size() == 1);
	CHECK(edited->entities[0].name == "EditedHero");
}

TEST_CASE("Unknown asset GUIDs fail references but do not reinterpret script text")
{
	constexpr auto missing = "uid://00000000000000000000000000000000";
	CHECK_FALSE(ParseToml(std::string("[[entities]]\nname='Hero'\n[entities.ui_image]\ntexture='") + missing + "'\n").has_value());
	const auto text = std::string("[[entities]]\nname='Scripted'\n[[entities.scripts]]\ntype='Message'\n[entities.scripts.properties.Message]\nt='string'\nv='") + missing + "'\n";
	const auto parsed = ParseToml(text);
	REQUIRE(parsed.has_value());
	REQUIRE(parsed->entities.size() == 1);
	REQUIRE(parsed->entities[0].scripts.size() == 1);
	CHECK(parsed->entities[0].scripts[0].properties.at("Message").str == missing);
	const auto restored = ParseToml(WriteToml(*parsed));
	REQUIRE(restored.has_value());
	CHECK(restored->entities[0].scripts[0].properties.at("Message").str == missing);
}

TEST_CASE("Binary tile references retain identity through migration move and packaged loading")
{
	const auto root = std::filesystem::temp_directory_path() / ("ae-tiles-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const auto package = root.string() + ".pak";

	struct Cleanup
	{
		std::filesystem::path root;
		std::filesystem::path package;

		~Cleanup()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::remove(package, ignored);
		}
	} cleanup{root, package};

	std::filesystem::create_directories(root / "assets");
	std::ofstream(root / "assets/terrain.tileset.toml") << "[tileset]\nname='Terrain'\ncell_size=1.0\ntile_count=0\n";
	TileMapAsset map;
	map.tileSetPath = "project://assets/terrain.tileset.toml";
	map.tilePalette.push_back(AssetObjectId{42});
	map.layers.emplace_back();
	map.SetCell(0, {-3, 6}, tilecell::Make(0, true, false));
	REQUIRE(map.SaveRaw(root / "assets/level.tiles").has_value());
	REQUIRE(AssetCatalog::Migrate(root, true).has_value());
	auto catalog = AssetCatalog::Load(root);
	REQUIRE(catalog.has_value());
	const auto tileSet = catalog->Reference("project://assets/terrain.tileset.toml");
	const auto tileMap = catalog->Reference("project://assets/level.tiles");
	REQUIRE(catalog->Move("assets/terrain.tileset.toml", "assets/renamed.tileset.toml").has_value());
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("project", root);
	const auto loose = TileMapAsset::Load(root / "assets/level.tiles");
	REQUIRE(loose.has_value());
	CHECK(loose->tileSetPath == tileSet);
	CHECK(loose->GetCell(0, {-3, 6}) == tilecell::Make(0, true, false));
	const auto packed = assetpipeline::PackProject(root, package, {});
	REQUIRE_MESSAGE(packed.ok, packed.message);
	io::FileSystem::MountPak("project", package);
	const auto restored = TileMapAsset::Load(std::filesystem::path(tileMap));
	REQUIRE(restored.has_value());
	CHECK(restored->tileSetPath == tileSet);
	CHECK(io::FileSystem::ResolveAssetPath(restored->tileSetPath) == "project://assets/renamed.tileset.toml");
	REQUIRE(restored->tilePalette.size() == 1);
	CHECK(restored->tilePalette[0] == AssetObjectId{42});
	CHECK(restored->GetCell(0, {-3, 6}) == tilecell::Make(0, true, false));
}
