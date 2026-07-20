#include <doctest/doctest.h>

#include <filesystem>

#include "assets/SpriteAssetStore.hpp"
#include "assets/SpriteAtlasAsset.hpp"
#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "rendering/RenderFramePacket.hpp"
#include "rendering/SpriteSystem.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/reflection/Reflection.hpp"
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
    aether::Finalize2DFrame(packet);
	REQUIRE(packet.sprites.size() == 2);
	CHECK(packet.sprites[0].entityId == lower.id);
	CHECK(packet.sprites[1].entityId == upper.id);
	CHECK(packet.sprites[1].sizeAndPivot.x == doctest::Approx(2.0f));
	CHECK((static_cast<std::uint32_t>(packet.sprites[1].flags) & static_cast<std::uint32_t>(SpriteInstanceFlags::FlipX)) != 0u);
	sprites.Shutdown();
	textures.ReleaseAll();
}

TEST_CASE("atlas sprite extraction samples inside the region texel borders")
{
	const std::filesystem::path atlasPath = std::filesystem::temp_directory_path() / "aethercore_sprite_uv_inset.spriteatlas.toml";
	std::filesystem::remove(atlasPath);

	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	SpriteAssetStore assets;
	SpriteAtlasAsset atlas = SpriteAtlasAsset::WholeTexture("atlas.png", {8.0f, 4.0f});
	REQUIRE(atlas.sprites.size() == 1);
	const AssetObjectId spriteId = atlas.sprites[0].id;
	REQUIRE(assets.SaveAtlas(atlasPath.string(), std::move(atlas)).has_value());

	SpriteSystem sprites;
	sprites.Initialize(textures, assets);
	World world;
	const Entity entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	world.Emplace<SpriteRendererComponent>(entity, SpriteRendererComponent{
	        .atlasPath = atlasPath.string(),
	        .spriteId = spriteId,
	});

	Render2DFrameData packet;
	sprites.Extract(world, packet);
    aether::Finalize2DFrame(packet);
	REQUIRE(packet.sprites.size() == 1);
	CHECK(packet.sprites[0].uvRect.x == doctest::Approx(0.0625f));
	CHECK(packet.sprites[0].uvRect.y == doctest::Approx(0.125f));
	CHECK(packet.sprites[0].uvRect.z == doctest::Approx(0.9375f));
	CHECK(packet.sprites[0].uvRect.w == doctest::Approx(0.875f));

	sprites.Shutdown();
	textures.ReleaseAll();
	std::filesystem::remove(atlasPath);
}

TEST_CASE("atlas sub-region extraction produces begin/end uv coordinates")
{
	// Regression: SpriteRegion.uvRect must be (x0, y0, x1, y1) — the sprite
	// shader lerps between xy and zw, so width/height in zw renders every
	// non-origin region wrong (tiles were the first sub-region consumer).
	const std::filesystem::path atlasPath = std::filesystem::temp_directory_path() / "aethercore_sprite_uv_subregion.spriteatlas.toml";
	std::filesystem::remove(atlasPath);

	FakeTextureSink sink;
	TextureRegistry textures(sink);
	textures.InitializeDefault("fallback.png");
	SpriteAssetStore assets;
	SpriteAtlasAsset atlas;
	atlas.texturePath = "atlas.png";
	atlas.textureWidth = 256;
	atlas.textureHeight = 128;
	SpriteRegion& region = atlas.AddManualRegion({64, 32, 64, 32}, "Cell");
	const AssetObjectId spriteId = region.id;
	atlas.RecalculateUvs(atlas.textureWidth, atlas.textureHeight);
	REQUIRE(assets.SaveAtlas(atlasPath.string(), std::move(atlas)).has_value());

	SpriteSystem sprites;
	sprites.Initialize(textures, assets);
	World world;
	const Entity entity = world.Create();
	world.Emplace<TransformComponent>(entity);
	world.Emplace<SpriteRendererComponent>(entity, SpriteRendererComponent{
	        .atlasPath = atlasPath.string(),
	        .spriteId = spriteId,
	});

	Render2DFrameData packet;
	sprites.Extract(world, packet);
	aether::Finalize2DFrame(packet);
	REQUIRE(packet.sprites.size() == 1);
	// Region (64,32,64,32) in a 256x128 texture: begin (0.25, 0.25), end
	// (0.5, 0.5), then a half-texel inset on each side.
	const float insetX = 0.5f / 256.0f;
	const float insetY = 0.5f / 128.0f;
	CHECK(packet.sprites[0].uvRect.x == doctest::Approx(0.25f + insetX));
	CHECK(packet.sprites[0].uvRect.y == doctest::Approx(0.25f + insetY));
	CHECK(packet.sprites[0].uvRect.z == doctest::Approx(0.5f - insetX));
	CHECK(packet.sprites[0].uvRect.w == doctest::Approx(0.5f - insetY));

	sprites.Shutdown();
	textures.ReleaseAll();
	std::filesystem::remove(atlasPath);
}

TEST_CASE("reflected 'sprite' field selects atlas sprites by name or index")
{
	// Agents pick atlas sub-region sprites through set_component with a
	// human-readable name (or index); the reflection setter resolves it
	// against the component's atlas and updates the stable spriteId.
	const std::filesystem::path atlasPath = std::filesystem::temp_directory_path() / "aethercore_sprite_field.spriteatlas.toml";
	std::filesystem::remove(atlasPath);

	SpriteAssetStore assets;
	SpriteAtlasAsset atlas;
	atlas.texturePath = "atlas.png";
	atlas.textureWidth = 64;
	atlas.textureHeight = 32;
	// AddManualRegion returns a reference into the sprites vector; a later add
	// may reallocate, so take ids from the vector after both adds.
	(void) atlas.AddManualRegion({0, 0, 32, 32}, "Idle");
	(void) atlas.AddManualRegion({32, 0, 32, 32}, "Run");
	const AssetObjectId idleId = atlas.sprites[0].id;
	const AssetObjectId runId = atlas.sprites[1].id;
	atlas.RecalculateUvs(atlas.textureWidth, atlas.textureHeight);
	REQUIRE(assets.SaveAtlas(atlasPath.string(), std::move(atlas)).has_value());

	const auto* type = aether::reflect::FindComponentType("Sprite Renderer");
	REQUIRE(type != nullptr);
	const auto* field = type->FindField("sprite");
	REQUIRE(field != nullptr);
	CHECK_FALSE(field->meta.serialize);

	SpriteRendererComponent sprite;
	sprite.atlasPath = atlasPath.string();

	// By name.
	field->set(&sprite, aether::reflect::MakeValue(std::string{"Run"}));
	CHECK(sprite.spriteId == runId);
	CHECK(sprite.texturePath == "atlas.png");
	CHECK(sprite.pixelSize.x == doctest::Approx(32.0f));

	// Read-back returns the name.
	CHECK(field->get(&sprite).str == "Run");

	// By index.
	field->set(&sprite, aether::reflect::MakeValue(std::string{"0"}));
	CHECK(sprite.spriteId == idleId);
	CHECK(field->get(&sprite).str == "Idle");

	// Unknown names leave the selection untouched; empty clears it.
	field->set(&sprite, aether::reflect::MakeValue(std::string{"Missing"}));
	CHECK(sprite.spriteId == idleId);
	field->set(&sprite, aether::reflect::MakeValue(std::string{}));
	CHECK_FALSE(sprite.spriteId.IsValid());

	std::filesystem::remove(atlasPath);
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
