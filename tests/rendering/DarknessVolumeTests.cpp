// WorldRenderer::GatherDarknessVolumes: the packing DarknessVolume.slangh reads.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "rendering/FrameConstants.hpp"
#include "rendering/WorldRenderer.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	Entity AddVolume(World& world, glm::vec3 pos, float yawDeg, glm::vec3 scale, float fade)
	{
		const Entity e = world.Create();
		world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(pos, glm::vec3(0.0f, yawDeg, 0.0f), scale)});
		world.Emplace<DarknessVolumeComponent>(e, DarknessVolumeComponent{.fadeDepth = fade});
		return e;
	}
} // namespace

TEST_CASE("GatherDarknessVolumes packs centre, fade, half extents and a yaw the shader can undo")
{
	World world;
	const glm::vec3 pos{10.0f, -5.0f, 3.0f};
	const Entity e = AddVolume(world, pos, 30.0f, glm::vec3(4.0f, 10.0f, 2.0f), 7.0f);
	std::vector<glm::vec4> out;
	WorldRenderer::GatherDarknessVolumes(world, glm::vec3(0.0f), out);
	REQUIRE(out.size() == 2u);
	CHECK(out[0].x == doctest::Approx(pos.x));
	CHECK(out[0].y == doctest::Approx(pos.y));
	CHECK(out[0].z == doctest::Approx(pos.z));
	CHECK(out[0].w == doctest::Approx(7.0f));
	CHECK(out[1].x == doctest::Approx(2.0f));
	CHECK(out[1].y == doctest::Approx(5.0f));
	CHECK(out[1].z == doctest::Approx(1.0f));

	// The shader takes a world point into the box as (c dx - s dz, dy, s dx + c dz) with the
	// packed yaw. The centre of the box's +x face must land on (half.x, 0, 0) that way, or the
	// darkness would be cast by a box turned the wrong way.
	const glm::mat4& m = world.Get<TransformComponent>(e).localToWorld;
	const glm::vec3 face = glm::vec3(m * glm::vec4(0.5f, 0.0f, 0.0f, 1.0f)) - pos;
	const float c = std::cos(out[1].w);
	const float s = std::sin(out[1].w);
	CHECK(c * face.x - s * face.z == doctest::Approx(2.0f));
	CHECK(s * face.x + c * face.z == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("GatherDarknessVolumes keeps the nearest volumes when there are more than the GPU holds")
{
	World world;
	for (std::uint32_t i = 0; i < kMaxDarknessVolumes + 4u; ++i)
	{
		AddVolume(world, glm::vec3(100.0f - static_cast<float>(i), 0.0f, 0.0f), 0.0f, glm::vec3(1.0f), 1.0f);
	}
	// A degenerate box casts nothing and must not take a slot.
	AddVolume(world, glm::vec3(0.0f), 0.0f, glm::vec3(0.0f), 1.0f);
	std::vector<glm::vec4> out;
	WorldRenderer::GatherDarknessVolumes(world, glm::vec3(0.0f), out);
	REQUIRE(out.size() == kMaxDarknessVolumes * 2u);
	// Nearest first: x = 100 - i, so the eye at the origin wants the largest i first.
	CHECK(out[0].x == doctest::Approx(100.0f - static_cast<float>(kMaxDarknessVolumes + 3u)));
	CHECK(out[2u * (kMaxDarknessVolumes - 1u)].x == doctest::Approx(100.0f - 4.0f));
}
