// scene.transform composed a whole matrix from parts with identity defaults, so setting an
// entity's position threw away the rotation and scale the caller never mentioned. That is
// silent data loss on the most ordinary call the method has, and it was only caught by
// driving a live editor - hence the merge lives here where it can be pinned.
#include <doctest/doctest.h>

#include "scene/TransformUtils.hpp"

namespace
{
	struct Trs
	{
		glm::vec3 pos{};
		glm::vec3 euler{};
		glm::vec3 scale{};
	};

	Trs Decompose(const glm::mat4& m)
	{
		Trs out;
		aether::DecomposeTRS(m, out.pos, out.euler, out.scale);
		return out;
	}

	// Euler/scale survive a decompose-recompose only to float precision.
	constexpr double kEps = 1e-4;
} // namespace

TEST_CASE("A partial transform keeps the parts it was not given")
{
	const glm::mat4 current = aether::ComposeTransform({1.0f, 2.0f, 3.0f}, {0.0f, 45.0f, 0.0f}, {3.0f, 3.0f, 3.0f});

	SUBCASE("position only")
	{
		const Trs t = Decompose(aether::ComposeTransformOver(current, {.position = glm::vec3{9.0f, 9.0f, 9.0f}}));
		CHECK(t.pos.x == doctest::Approx(9.0));
		CHECK(t.euler.y == doctest::Approx(45.0).epsilon(kEps));
		CHECK(t.scale.y == doctest::Approx(3.0).epsilon(kEps));
	}

	SUBCASE("scale only")
	{
		const Trs t = Decompose(aether::ComposeTransformOver(current, {.scale = glm::vec3{5.0f, 5.0f, 5.0f}}));
		CHECK(t.pos.x == doctest::Approx(1.0));
		CHECK(t.euler.y == doctest::Approx(45.0).epsilon(kEps));
		CHECK(t.scale.y == doctest::Approx(5.0).epsilon(kEps));
	}

	SUBCASE("rotation only")
	{
		const Trs t = Decompose(aether::ComposeTransformOver(current, {.eulerDeg = glm::vec3{0.0f, 90.0f, 0.0f}}));
		CHECK(t.pos.z == doctest::Approx(3.0));
		CHECK(t.euler.y == doctest::Approx(90.0).epsilon(kEps));
		CHECK(t.scale.y == doctest::Approx(3.0).epsilon(kEps));
	}

	SUBCASE("nothing at all is a no-op")
	{
		const Trs t = Decompose(aether::ComposeTransformOver(current, {}));
		CHECK(t.pos.y == doctest::Approx(2.0));
		CHECK(t.euler.y == doctest::Approx(45.0).epsilon(kEps));
		CHECK(t.scale.y == doctest::Approx(3.0).epsilon(kEps));
	}

	SUBCASE("all three replaces everything")
	{
		const Trs t = Decompose(aether::ComposeTransformOver(current,
		        {.position = glm::vec3{0.0f}, .eulerDeg = glm::vec3{0.0f}, .scale = glm::vec3{1.0f}}));
		CHECK(t.pos.x == doctest::Approx(0.0));
		CHECK(t.euler.y == doctest::Approx(0.0).epsilon(kEps));
		CHECK(t.scale.y == doctest::Approx(1.0).epsilon(kEps));
	}
}
