#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "animation/SpriteAnimationSystem.hpp"
#include "assets/SpriteAnimationAsset.hpp"
#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "editor/AsepriteSpriteImporter.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	std::filesystem::path TestAssetPath(std::string_view name)
	{
		return std::filesystem::temp_directory_path() / ("aether-sprite-test-" + std::string(name));
	}
}

TEST_CASE("sprite grid slicing preserves stable IDs and trims alpha bounds")
{
	std::vector<std::uint8_t> pixels(8u * 4u * 4u, 0u);
	for (std::uint32_t y = 1; y < 3; ++y)
	{
		for (std::uint32_t x = 1; x < 3; ++x)
		{
			pixels[(y * 8u + x) * 4u + 3u] = 255u;
		}
	}
	SpriteSliceSettings settings;
	settings.cellWidth = 4;
	settings.cellHeight = 4;
	settings.trimAlpha = true;
	const SpriteAtlasAsset first = SpriteAtlasAsset::SliceGrid("assets://sheet.png", 8, 4, settings, nullptr, pixels);
	REQUIRE(first.sprites.size() == 2);
	CHECK(first.sprites[0].pixelRect == (SpritePixelRect{1, 1, 2, 2}));

	SpriteSliceSettings edited = settings;
	edited.trimAlpha = false;
	const SpriteAtlasAsset second = SpriteAtlasAsset::SliceGrid("assets://sheet.png", 8, 4, edited, &first, pixels);
	REQUIRE(second.sprites.size() == first.sprites.size());
	CHECK(second.sprites[0].id == first.sprites[0].id);
	CHECK(second.sprites[1].id == first.sprites[1].id);
	const SpriteAtlasReimportDiagnostics diagnostics = CompareSpriteAtlasReimport(first, second);
	CHECK(diagnostics.preserved == 2);
	CHECK(diagnostics.added == 0);
	CHECK(diagnostics.removed == 0);
}

TEST_CASE("sprite atlas and animation assets round trip stable references")
{
	const std::filesystem::path atlasPath = TestAssetPath("roundtrip.spriteatlas.toml");
	const std::filesystem::path animationPath = TestAssetPath("roundtrip.spriteanim.toml");
	SpriteAtlasAsset atlas = SpriteAtlasAsset::WholeTexture("assets://hero.png", {64.0f, 32.0f}, 32.0f, "Hero");
	atlas.sliceSettings.origin = SpriteSliceOrigin::BottomLeft;
	atlas.sprites[0].pivot = {0.25f, 0.75f};
	atlas.sprites[0].collisionOutline = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
	REQUIRE(atlas.Save(atlasPath).has_value());
	const auto loadedAtlas = SpriteAtlasAsset::Load(atlasPath);
	REQUIRE(loadedAtlas.has_value());
	REQUIRE(loadedAtlas->sprites.size() == 1);
	CHECK(loadedAtlas->sprites[0].id == atlas.sprites[0].id);
	CHECK(loadedAtlas->sprites[0].pivot == atlas.sprites[0].pivot);
	CHECK(loadedAtlas->sprites[0].collisionOutline.size() == 3);
	CHECK(loadedAtlas->sliceSettings.origin == SpriteSliceOrigin::BottomLeft);

	SpriteAnimationAsset animation;
	animation.name = "Idle";
	animation.atlasPath = atlasPath.string();
	animation.loopMode = SpriteAnimationLoopMode::PingPong;
	animation.frames = {{atlas.sprites[0].id, 0.075f}, {AssetObjectId{42}, 0.125f}};
	animation.events = {{1, "Footstep", "stone"}};
	REQUIRE(animation.Save(animationPath).has_value());
	const auto loadedAnimation = SpriteAnimationAsset::Load(animationPath);
	REQUIRE(loadedAnimation.has_value());
	CHECK(loadedAnimation->loopMode == SpriteAnimationLoopMode::PingPong);
	CHECK(loadedAnimation->frames[0].spriteId == atlas.sprites[0].id);
	CHECK(loadedAnimation->events[0].payload == "stone");

	std::error_code error;
	std::filesystem::remove(atlasPath, error);
	std::filesystem::remove(animationPath, error);
}

TEST_CASE("Aseprite metadata imports stable regions and tagged animation clips")
{
	const std::filesystem::path metadataPath = TestAssetPath("aseprite.json");
	{
		std::ofstream output(metadataPath);
		output << R"json({
			"frames": {
				"hero_0": {"frame": {"x": 0, "y": 0, "w": 16, "h": 16}, "duration": 80},
				"hero_1": {"frame": {"x": 16, "y": 0, "w": 16, "h": 16}, "duration": 120}
			},
			"meta": {
				"size": {"w": 32, "h": 16},
				"frameTags": [{"name": "Run", "from": 0, "to": 1, "direction": "pingpong"}]
			}
		})json";
	}
	const auto first = editor::ImportAsepriteSpriteMetadata(metadataPath, "project://textures/hero.png");
	REQUIRE(first.has_value());
	REQUIRE(first->atlas.sprites.size() == 2);
	REQUIRE(first->animations.size() == 1);
	CHECK(first->animations[0].loopMode == SpriteAnimationLoopMode::PingPong);
	CHECK(first->animations[0].frames[0].durationSeconds == doctest::Approx(0.08f));
	CHECK(first->animations[0].frames[1].durationSeconds == doctest::Approx(0.12f));

	SpriteAtlasAsset edited = first->atlas;
	edited.sprites[0].name = "Hero Idle";
	edited.sprites[0].pivot = {0.25f, 0.75f};
	const auto reimported = editor::ImportAsepriteSpriteMetadata(metadataPath, "project://textures/hero.png", &edited);
	REQUIRE(reimported.has_value());
	CHECK(reimported->atlas.sprites[0].id == edited.sprites[0].id);
	CHECK(reimported->atlas.sprites[0].name == "Hero Idle");
	CHECK(reimported->atlas.sprites[0].pivot == glm::vec2(0.25f, 0.75f));

	std::error_code error;
	std::filesystem::remove(metadataPath, error);
}

TEST_CASE("sprite animation advances on fixed steps and buffers frame events")
{
	const std::filesystem::path atlasPath = TestAssetPath("runtime.spriteatlas.toml");
	const std::filesystem::path animationPath = TestAssetPath("runtime.spriteanim.toml");
	SpriteAtlasAsset atlas = SpriteAtlasAsset::SliceGrid("assets://runtime.png", 64, 32, SpriteSliceSettings{.cellWidth = 32, .cellHeight = 32});
	REQUIRE(atlas.sprites.size() == 2);
	SpriteAnimationAsset animation;
	animation.atlasPath = atlasPath.string();
	animation.frames = {{atlas.sprites[0].id, 0.05f}, {atlas.sprites[1].id, 0.05f}};
	animation.events = {{1, "Step", "right"}};

	SpriteAssetStore store;
	REQUIRE(store.SaveAtlas(atlasPath.string(), atlas).has_value());
	REQUIRE(store.SaveAnimation(animationPath.string(), animation).has_value());
	SpriteAnimationSystem system(store);
	World world;
	const Entity entity = world.Create();
	world.Emplace<SpriteRendererComponent>(entity);
	world.Emplace<SpriteAnimatorComponent>(entity, SpriteAnimatorComponent{.animationPath = animationPath.string()});

	system.Update(world, 0.02f);
	CHECK(world.Get<SpriteAnimatorComponent>(entity).currentFrame == 0);
	system.Update(world, 0.05f);
	CHECK(world.Get<SpriteAnimatorComponent>(entity).currentFrame == 1);
	CHECK(world.Get<SpriteRendererComponent>(entity).spriteId == atlas.sprites[1].id);
	SpriteAnimationEventRecord event;
	REQUIRE(system.TryPopEvent(entity, event));
	CHECK(event.name == "Step");
	CHECK(event.payload == "right");

	for (std::uint32_t i = 0; i < 100; ++i)
	{
		system.Update(world, 0.25f);
	}
	CHECK(system.PendingEventCount() <= SpriteAnimationSystem::kMaxQueuedEvents);

	std::error_code error;
	std::filesystem::remove(atlasPath, error);
	std::filesystem::remove(animationPath, error);
}
