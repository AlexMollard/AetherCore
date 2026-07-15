#include <doctest/doctest.h>

#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/SpriteSystem.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"
#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;

TEST_CASE("whole-texture sprite atlas creates stable authored identity")
{
	const SpriteAtlasAsset first = SpriteAtlasAsset::WholeTexture("project://textures/hero.png", {256.0f, 128.0f}, 64.0f, "Hero");
	const SpriteAtlasAsset second = SpriteAtlasAsset::WholeTexture("project://textures/hero.png", {256.0f, 128.0f}, 64.0f, "Renamed");
	REQUIRE(first.sprites.size() == 1);
	CHECK(first.sprites[0].id.IsValid());
	CHECK(first.sprites[0].id == second.sprites[0].id);
	CHECK(first.sprites[0].uvRect == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
	CHECK(first.sprites[0].pixelSize == glm::vec2(256.0f, 128.0f));
}

TEST_CASE("sprite extraction is packet-owned and deterministically sorted")
{
	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	SpriteAssetStore assets;
	SpriteSystem sprites;
	sprites.Initialize(textures, assets);
	World world;

	const Entity upper = world.Create();
	world.Emplace<TransformComponent>(upper);
	world.Emplace<SpriteRendererComponent>(upper, SpriteRendererComponent{
	        .texturePath = "upper.png",
	        .pixelSize = {200.0f, 100.0f},
	        .pixelsPerUnit = 100.0f,
	        .sortingLayer = 2,
	        .orderInLayer = 1,
	        .flipX = true,
	});
	const Entity lower = world.Create();
	world.Emplace<TransformComponent>(lower);
	world.Emplace<SpriteRendererComponent>(lower, SpriteRendererComponent{
	        .texturePath = "lower.png",
	        .sortingLayer = -1,
	        .orderInLayer = 9,
	});

	Render2DFrameData packet;
	sprites.Extract(world, packet);
	REQUIRE(packet.sprites.size() == 2);
	CHECK(packet.sprites[0].entityId == lower.id);
	CHECK(packet.sprites[1].entityId == upper.id);
	CHECK(packet.sprites[1].sizeAndPivot.x == doctest::Approx(2.0f));
	CHECK((static_cast<std::uint32_t>(packet.sprites[1].flags) & static_cast<std::uint32_t>(SpriteInstanceFlags::FlipX)) != 0u);
	sprites.Shutdown();
	textures.ReleaseAll();
}

TEST_CASE("authored sprite renderer survives scene serialization and legacy marker migration")
{
	FakeSlotSink materialSink(8);
	FakeTextureSink textureSink;
	TextureRegistry textures(textureSink);
	MaterialRegistry materials(materialSink, textures);
	World world;
	const Entity entity = world.Create();
	world.Emplace<NameComponent>(entity, NameComponent{.name = "Sprite"});
	world.Emplace<TransformComponent>(entity);
	world.Emplace<SpriteRendererComponent>(entity, SpriteRendererComponent{
	        .texturePath = "project://textures/hero.png",
	        .uvRect = {0.1f, 0.2f, 0.7f, 0.9f},
	        .tint = {0.2f, 0.4f, 0.8f, 0.75f},
	        .pixelSize = {64.0f, 32.0f},
	        .pivot = {0.25f, 0.75f},
	        .pixelsPerUnit = 32.0f,
	        .sortingLayer = 3,
	        .orderInLayer = -2,
	        .blendMode = SpriteBlendMode::Additive,
	        .flipY = true,
	});
	world.Emplace<SpriteAnimatorComponent>(entity, SpriteAnimatorComponent{
	        .animationPath = "project://animations/hero-idle.spriteanim.toml",
	        .speed = 1.25f,
	        .startFrame = 2,
	        .loopMode = SpriteAnimationLoopMode::PingPong,
	        .useAssetLoopMode = false,
	        .autoplay = false,
	});

	const auto captured = app::scene::CaptureScene(world, materials, textures);
	const auto parsed = app::scene::ParseToml(app::scene::WriteToml(captured));
	REQUIRE(parsed.has_value());
	REQUIRE(parsed->entities.size() == 1);
	REQUIRE(parsed->entities[0].sprite.has_value());
	CHECK(parsed->entities[0].sprite->texturePath == "project://textures/hero.png");
	CHECK(parsed->entities[0].sprite->sortingLayer == 3);
	CHECK(parsed->entities[0].sprite->orderInLayer == -2);
	CHECK(parsed->entities[0].sprite->blendMode == SpriteBlendMode::Additive);
	CHECK(parsed->entities[0].sprite->flipY);
	REQUIRE(parsed->entities[0].spriteAnimator.has_value());
	CHECK(parsed->entities[0].spriteAnimator->animationPath == "project://animations/hero-idle.spriteanim.toml");
	CHECK(parsed->entities[0].spriteAnimator->speed == doctest::Approx(1.25f));
	CHECK(parsed->entities[0].spriteAnimator->startFrame == 2);
	CHECK(parsed->entities[0].spriteAnimator->loopMode == SpriteAnimationLoopMode::PingPong);
	CHECK_FALSE(parsed->entities[0].spriteAnimator->useAssetLoopMode);
	CHECK_FALSE(parsed->entities[0].spriteAnimator->autoplay);

	const auto clipboard = app::scene::ParseToml(app::scene::WriteToml(app::scene::CaptureSubtrees(world, {entity}, materials, textures)));
	REQUIRE(clipboard.has_value());
	REQUIRE(clipboard->entities.size() == 1);
	REQUIRE(clipboard->entities[0].sprite.has_value());
	CHECK(clipboard->entities[0].sprite->texturePath == "project://textures/hero.png");
	CHECK(clipboard->entities[0].sprite->sortingLayer == 3);
	REQUIRE(clipboard->entities[0].spriteAnimator.has_value());
	CHECK(clipboard->entities[0].spriteAnimator->startFrame == 2);

	const auto legacy = app::scene::ParseToml("version = 10\nname = 'Legacy'\n[[entities]]\nname = 'Old Sprite'\nsprite = true\n");
	REQUIRE(legacy.has_value());
	REQUIRE(legacy->entities[0].sprite.has_value());
	CHECK(legacy->entities[0].sprite->visible);
}
