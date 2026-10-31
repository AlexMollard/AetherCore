#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "assets/AssetCatalog.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "io/FileSystem.hpp"
#include "io/FileUtil.hpp"
#include "scene/SceneSerializer.hpp"
#include "material/TextureRegistry.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;

namespace
{
	struct CatalogProject
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / ("ae-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

		CatalogProject()
		{
			std::filesystem::create_directories(root / "assets/textures");
			std::filesystem::create_directories(root / "scenes");
			std::ofstream(root / "assets/textures/hero.png", std::ios::binary) << "original-image-bytes";
			std::ofstream(root / "scenes/start.scene.toml", std::ios::binary) << "[scene]\nversion = 16\nname = 'Start'\n[[entities]]\nname = 'Hero'\n[entities.ui_image]\ntexture = 'project://assets/textures/hero.png'\n";
		}

		~CatalogProject()
		{
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	};

	std::string CatalogText(const std::filesystem::path& path)
	{
		const auto text = io::file_util::ReadText(path);
		REQUIRE_MESSAGE(text.has_value(), path.string());
		return *text;
	}
} // namespace

TEST_CASE("Asset migration preview leaves legacy projects byte identical")
{
	CatalogProject project;
	const auto before = io::file_util::ReadText(project.root / "scenes/start.scene.toml");
	REQUIRE(before.has_value());
	const auto preview = AssetCatalog::Migrate(project.root, false);
	REQUIRE(preview.has_value());
	CHECK(CatalogText(project.root / "scenes/start.scene.toml") == *before);
	CHECK_FALSE(std::filesystem::exists(project.root / "asset-catalog.toml"));
	CHECK_FALSE(std::filesystem::exists(project.root / "assets/textures/hero.png.uid"));
	CHECK_FALSE(AssetCatalog::Load(project.root).has_value());
}

TEST_CASE("Migrated project references survive file and folder moves restart and reimport")
{
	CatalogProject project;
	const auto legacy = io::file_util::ReadText(project.root / "scenes/start.scene.toml");
	const auto migration = AssetCatalog::Migrate(project.root, true);
	REQUIRE(migration.has_value());
	CHECK(std::filesystem::exists(migration->backupDirectory));
	REQUIRE(legacy.has_value());
	CHECK(CatalogText(migration->backupDirectory / "scenes/start.scene.toml") == *legacy);
	const auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	const auto reference = catalog->Reference("project://assets/textures/hero.png");
	REQUIRE(reference.starts_with("uid://"));
	const auto refs = catalog->References(reference);
	REQUIRE(refs.has_value());
	REQUIRE(refs->size() == 1);
	CHECK((*refs)[0].document == "scenes/start.scene.toml");
	CHECK(catalog->Resolve(reference) == "project://assets/textures/hero.png");
	const auto encoded = io::file_util::ReadText(project.root / "scenes/start.scene.toml");
	REQUIRE(encoded.has_value());
	CHECK(encoded->find(reference) != std::string::npos);
	{
		auto moving = AssetCatalog::Load(project.root);
		REQUIRE(moving.has_value());
		REQUIRE(moving->Move("assets/textures/hero.png", "assets/textures/player.png").has_value());
		REQUIRE(moving->Move("assets/textures", "assets/art").has_value());
	}
	std::ofstream(project.root / "assets/art/player.png", std::ios::binary | std::ios::trunc) << "reimported-image-bytes";
	const auto restarted = AssetCatalog::Load(project.root);
	REQUIRE(restarted.has_value());
	CHECK(restarted->Reference("project://assets/art/player.png") == reference);
	CHECK(restarted->Resolve(reference) == "project://assets/art/player.png");
	CHECK(CatalogText(project.root / "scenes/start.scene.toml") == *encoded);
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("project", project.root);
	REQUIRE(io::FileSystem::ReloadAssetCatalog().has_value());
	const auto image = io::FileSystem::ReadFileText(reference);
	REQUIRE(image.has_value());
	CHECK(*image == "reimported-image-bytes");
	const auto scene = app::scene::ParseToml(*encoded);
	REQUIRE(scene.has_value());
	REQUIRE(scene->entities.size() == 1);
	REQUIRE(scene->entities[0].uiImage.has_value());
	CHECK(scene->entities[0].uiImage->texturePath == reference);
	const auto restored = app::scene::ParseToml(app::scene::WriteToml(*scene));
	REQUIRE(restored.has_value());
	REQUIRE(restored->entities[0].uiImage.has_value());
	CHECK(restored->entities[0].uiImage->texturePath == reference);
}

TEST_CASE("Non-asset project files can be renamed and moved without changing asset identities")
{
	CatalogProject project;
	std::filesystem::create_directories(project.root / "scripts/src");
	std::filesystem::create_directories(project.root / "scripts/nested");
	std::ofstream(project.root / "scripts/Player.cs") << "class Player {}";
	std::ofstream(project.root / "scripts/src/player.rs") << "struct Player;";
	std::ofstream(project.root / "notes.txt") << "project notes";
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	const auto persisted = CatalogText(project.root / "asset-catalog.toml");
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	for (const auto& [source, target]: {std::pair{"scripts/Player.cs", "scripts/Hero.cs"}, std::pair{"scripts/Hero.cs", "scripts/nested/Hero.cs"},
	             std::pair{"scripts/src/player.rs", "scripts/src/hero.rs"}, std::pair{"notes.txt", "scripts/nested/notes.txt"}})
	{
		const auto before = CatalogText(project.root / source);
		const auto moved = catalog->Move(source, target);
		REQUIRE_MESSAGE(moved.has_value(), (moved ? "" : moved.error().message));
		CHECK_FALSE(std::filesystem::exists(project.root / source));
		CHECK(CatalogText(project.root / target) == before);
		CHECK_FALSE(std::filesystem::exists(project.root / (std::string(target) + ".uid")));
	}
	CHECK(CatalogText(project.root / "asset-catalog.toml") == persisted);
	REQUIRE(AssetCatalog::Load(project.root).has_value());
}

TEST_CASE("Empty and script-only project folders can be renamed and moved")
{
	CatalogProject project;
	std::filesystem::create_directories(project.root / "empty");
	std::filesystem::create_directories(project.root / "scripts/src");
	std::filesystem::create_directories(project.root / "destination");
	std::ofstream(project.root / "scripts/src/player.rs") << "struct Player;";
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	const auto persisted = CatalogText(project.root / "asset-catalog.toml");
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	REQUIRE(catalog->Move("empty", "renamed").has_value());
	REQUIRE(catalog->Move("renamed", "destination/renamed").has_value());
	CHECK(std::filesystem::is_empty(project.root / "destination/renamed"));
	CHECK_FALSE(std::filesystem::exists(project.root / "empty"));
	REQUIRE(catalog->Move("scripts/src", "scripts/gameplay").has_value());
	CHECK(CatalogText(project.root / "scripts/gameplay/player.rs") == "struct Player;");
	CHECK_FALSE(std::filesystem::exists(project.root / "scripts/src"));
	CHECK(CatalogText(project.root / "asset-catalog.toml") == persisted);
}

TEST_CASE("Non-asset moves preserve path collision and catalog metadata protections")
{
	CatalogProject project;
	std::ofstream(project.root / "notes.txt") << "project notes";
	std::ofstream(project.root / "occupied.txt") << "occupied";
	std::filesystem::create_directories(project.root / "empty/nested");
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	std::ofstream(project.root / "assets/unindexed.png") << "unindexed image";
	std::filesystem::create_directories(project.root / "unindexed");
	std::ofstream(project.root / "unindexed/hero.png") << "unindexed image";
	const auto persisted = CatalogText(project.root / "asset-catalog.toml");
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	CHECK_FALSE(catalog->Move("missing.txt", "renamed.txt").has_value());
	CHECK_FALSE(catalog->Move("notes.txt", "../escape.txt").has_value());
	CHECK_FALSE(catalog->Move("notes.txt", "occupied.txt").has_value());
	CHECK_FALSE(catalog->Move("notes.txt", "notes.uid").has_value());
	CHECK_FALSE(catalog->Move("empty", "empty/nested/moved").has_value());
	CHECK_FALSE(catalog->Move("assets/unindexed.png", "assets/renamed.png").has_value());
	CHECK_FALSE(catalog->Move("unindexed", "renamed").has_value());
	CHECK_FALSE(catalog->Move("asset-catalog.toml", "catalog.txt").has_value());
	CHECK_FALSE(catalog->Move("assets/textures/hero.png.uid", "identity.txt").has_value());
	CHECK(CatalogText(project.root / "notes.txt") == "project notes");
	CHECK(CatalogText(project.root / "occupied.txt") == "occupied");
	CHECK(CatalogText(project.root / "asset-catalog.toml") == persisted);
}

TEST_CASE("Asset moves protect metadata with alternate filename casing")
{
	CatalogProject project;
	std::ofstream(project.root / "notes.txt") << "project notes";
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	const auto persisted = CatalogText(project.root / "asset-catalog.toml");
	const auto identity = CatalogText(project.root / "assets/textures/hero.png.uid");
	CHECK_FALSE(catalog->Move("notes.txt", "notes.UID").has_value());
#ifdef _WIN32
	CHECK_FALSE(catalog->Move("assets/textures/hero.png.UID", "identity.txt").has_value());
	CHECK_FALSE(catalog->Move("ASSET-CATALOG.TOML", "catalog.txt").has_value());
#endif
	CHECK(CatalogText(project.root / "notes.txt") == "project notes");
	CHECK(CatalogText(project.root / "assets/textures/hero.png.uid") == identity);
	CHECK(CatalogText(project.root / "asset-catalog.toml") == persisted);
}

TEST_CASE("Unsafe asset moves leave source metadata and catalog unchanged")
{
	CatalogProject project;
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	std::ofstream(project.root / "assets/textures/existing.png") << "occupied";
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	const auto persisted = CatalogText(project.root / "asset-catalog.toml");
	const auto sidecar = CatalogText(project.root / "assets/textures/hero.png.uid");
	CHECK_FALSE(catalog->Move("assets/textures/hero.png", "assets/textures/existing.png").has_value());
	CHECK_FALSE(catalog->Move("assets/textures/hero.png", "../escape.png").has_value());
	CHECK_FALSE(catalog->Move("assets", "assets/nested").has_value());
	CHECK(CatalogText(project.root / "asset-catalog.toml") == persisted);
	CHECK(CatalogText(project.root / "assets/textures/hero.png.uid") == sidecar);
	CHECK(CatalogText(project.root / "assets/textures/hero.png") == "original-image-bytes");
	CHECK(CatalogText(project.root / "assets/textures/existing.png") == "occupied");
}

TEST_CASE("Catalog rejects future schema duplicate identities and escaping paths")
{
	CHECK_FALSE(AssetCatalog::Parse("version = 999\n").has_value());
	CHECK_FALSE(AssetCatalog::Parse("version = '1'\n").has_value());
	CHECK_FALSE(AssetCatalog::Parse("version=1\n[[assets]]\nuid='11111111111111111111111111111111'\npath='../outside.png'\n").has_value());
	CHECK_FALSE(AssetCatalog::Parse("version=1\n[[assets]]\nuid='11111111111111111111111111111111'\npath='a.png'\n[[assets]]\nuid='11111111111111111111111111111111'\npath='b.png'\n").has_value());
	const auto catalog = AssetCatalog::Parse("version=1\n[[assets]]\nuid='11111111111111111111111111111111'\npath='assets/hero.png'\n");
	REQUIRE(catalog.has_value());
	CHECK(catalog->Resolve("uid://22222222222222222222222222222222").empty());
}

TEST_CASE("Asset migration rejects unsupported source schemas without writes")
{
	const struct
	{
		const char* path;
		const char* text;
	} cases[] = {
	        {"scenes/start.scene.toml", "[scene]\nversion=19\n"},
	        {"scenes/start.scene.toml", "[scene]\nversion='17'\n"},
	        {"scenes/start.scene.toml", "[scene]\nversion=17\n[[entities]]\n[entities.ui_image]\nschema_version=2\n"},
	        {"assets/hero.prefab.toml", "[prefab]\nversion=2\n"},
	        {"assets/sprites.spriteatlas.toml", "[atlas]\nschema_version=4\n"},
	        {"assets/walk.spriteanimation.toml", "[animation]\nschema_version=3\n"},
	        {"assets/terrain.tileset.toml", "[tileset]\nschema_version=3\n"},
	        {"assets/properties.toml", "[material]\nschema_version=2\n"},
	};

	for (const auto& item: cases)
	{
		CatalogProject project;
		const auto path = project.root / item.path;
		std::ofstream(path, std::ios::binary | std::ios::trunc) << item.text;
		const auto before = CatalogText(path);
		for (const bool apply: {false, true})
		{
			CHECK_FALSE(AssetCatalog::Migrate(project.root, apply).has_value());
			CHECK(CatalogText(path) == before);
			CHECK_FALSE(std::filesystem::exists(project.root / "asset-catalog.toml"));
			CHECK_FALSE(std::filesystem::exists(project.root / "assets/textures/hero.png.uid"));
			CHECK_FALSE(std::filesystem::exists(project.root / ".asset-migration-backups"));
		}
	}
}

TEST_CASE("Texture handles and cooked path dedup survive a registered source rename")
{
	CatalogProject project;
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("project", project.root);
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	const auto reference = catalog->Reference("project://assets/textures/hero.png");
	FakeTextureSink sink(8);
	TextureRegistry textures(sink);
	const auto original = textures.Acquire(reference);
	REQUIRE(original.IsValid());
	REQUIRE(catalog->Move("assets/textures/hero.png", "assets/textures/player.png").has_value());
	REQUIRE(io::FileSystem::ReloadAssetCatalog().has_value());
	textures.RetargetPaths("project://assets/textures/hero.png", "project://assets/textures/player.png");
	CHECK(textures.Acquire(reference) == original);
	CHECK(textures.Acquire("project://assets/textures/player.texture") == original);
	CHECK(sink.loadCount == 1);
	std::string captured;
	REQUIRE(textures.TryGetPath(original, captured));
	CHECK(captured == reference);
}

TEST_CASE("Legacy float-valued sprite schemas migrate without losing texture references")
{
	CatalogProject project;
	const auto atlasPath = project.root / "assets/legacy.atlas.toml";
	std::ofstream(atlasPath) << "[atlas]\nschema_version=2.0\ntexture='project://assets/textures/hero.png'\ntexture_width=16\ntexture_height=16\nsprite_count=0\n";
	REQUIRE(AssetCatalog::Migrate(project.root, false).has_value());
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("project", project.root);
	const auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	const auto atlas = SpriteAtlasAsset::Load(atlasPath);
	REQUIRE(atlas.has_value());
	CHECK(atlas->schemaVersion == SpriteAtlasAsset::kSchemaVersion);
	CHECK(atlas->texturePath == catalog->Reference("project://assets/textures/hero.png"));
}

TEST_CASE("Stream region references preserve identity across chunk scene moves")
{
	CatalogProject project;
	const auto worldPath = project.root / "scenes/world.scene.toml";
	std::ofstream(worldPath, std::ios::binary)
	        << "[scene]\nversion=18\n[[stream_regions]]\nid='west'\nscene='start'\n"
	           "bounds_min=[-5.0,-5.0,-5.0]\nbounds_max=[5.0,5.0,5.0]\n";
	REQUIRE(AssetCatalog::Migrate(project.root, true).has_value());
	auto catalog = AssetCatalog::Load(project.root);
	REQUIRE(catalog.has_value());
	const auto reference = catalog->Reference("project://scenes/start.scene.toml");
	REQUIRE(reference.starts_with("uid://"));
	const auto worldBefore = CatalogText(worldPath);
	CHECK(worldBefore.find(reference) != std::string::npos);
	const auto references = catalog->References(reference);
	REQUIRE(references.has_value());
	REQUIRE(references->size() == 1);
	CHECK(references->front().document == "scenes/world.scene.toml");
	REQUIRE(catalog->Move("scenes/start.scene.toml", "scenes/renamed.scene.toml").has_value());
	CHECK(catalog->Resolve(reference) == "project://scenes/renamed.scene.toml");
	CHECK(CatalogText(worldPath) == worldBefore);
	if (!io::FileSystem::IsInitialized())
	{
		io::FileSystem::Initialize();
	}
	io::FileSystem::Mount("project", project.root);
	REQUIRE(io::FileSystem::ReloadAssetCatalog().has_value());
	const auto restored = app::scene::ParseToml(worldBefore);
	REQUIRE(restored.has_value());
	REQUIRE(restored->streamingRegions.size() == 1);
	CHECK(restored->streamingRegions.front().scene == reference);
	CHECK(restored->streamingRegions.front().boundsMin.x == -5.0f);
	CHECK(io::FileSystem::Exists(reference));
}
