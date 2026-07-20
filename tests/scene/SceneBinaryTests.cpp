// The binary scene format is the TOML document tree encoded as compact bytes, so
// it must round-trip byte-for-byte with the TOML path: for any scene,
// WriteToml(ReadSceneBinary(WriteSceneBinary(d))) == WriteToml(d). We build a
// scene exercising many component types (via the reflection-driven capture) and
// also drive a couple of hand-built descriptions.

#include <doctest/doctest.h>

#include <string>

#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"

using namespace aether;
using namespace aether::app::scene;

namespace
{
	// Round-trip through binary must reproduce the exact TOML text.
	void CheckBinaryRoundTrip(const SceneDescription& scene)
	{
		const std::string canonicalToml = WriteToml(scene);
		const std::vector<std::byte> binary = WriteSceneBinary(scene);
		const auto decoded = ReadSceneBinary(binary);
		REQUIRE(decoded.has_value());
		CHECK(WriteToml(*decoded) == canonicalToml);
	}
}

TEST_CASE("Binary scene format round-trips a populated scene")
{
	FakeSlotSink sink(16);
	FakeTextureSink tsink;
	TextureRegistry treg(tsink);
	MaterialRegistry mreg(sink, treg);
	World world;

	const Entity root = world.Create();
	world.Emplace<NameComponent>(root, NameComponent{.name = "Root"});
	world.Emplace<TransformComponent>(root, TransformComponent{});

	const Entity child = world.Create();
	world.Emplace<NameComponent>(child, NameComponent{.name = "Child \"quoted\" & unicode \xC3\xA9"});
	world.Emplace<TransformComponent>(child, TransformComponent{});
	world.Emplace<SpriteRendererComponent>(child, SpriteRendererComponent{});
	world.Emplace<SpinComponent>(child, SpinComponent{});
	world.Emplace<ParallaxComponent>(child, ParallaxComponent{});
	ecs::SetParent(world, child, root);

	const SceneDescription scene = CaptureScene(world, mreg, treg);
	REQUIRE(scene.entities.size() == 2);
	CheckBinaryRoundTrip(scene);
}

TEST_CASE("Binary scene format round-trips an empty scene")
{
	SceneDescription scene;
	scene.name = "Empty";
	CheckBinaryRoundTrip(scene);
}

TEST_CASE("Binary scene format round-trips lights and environment")
{
	SceneDescription scene;
	scene.name = "Lit";
	LightRecord light;
	light.isSpot = true;
	light.position = glm::vec3(1.0f, 2.0f, 3.0f);
	light.intensity = 4.5f;
	light.castsShadow = true;
	scene.lights.push_back(light);
	EnvironmentRecord env;
	env.ambient = glm::vec3(0.3f);
	env.sunIntensity = 2.0f;
	scene.environment = env;
	CheckBinaryRoundTrip(scene);
}

TEST_CASE("ReadSceneBinary rejects a bad header")
{
	const std::vector<std::byte> garbage(16, std::byte{0x7F});
	CHECK_FALSE(ReadSceneBinary(garbage).has_value());
	CHECK_FALSE(ReadSceneBinary(std::vector<std::byte>{}).has_value());
}
