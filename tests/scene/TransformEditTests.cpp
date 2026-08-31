#include <doctest/doctest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "debug/SelectionBounds.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/TransformEdit.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "scene/reflection/Reflection.hpp"

#include <array>
#include <cstdlib>

using namespace aether;

namespace
{
    World MakeWorld()
    {
        return World{};
    }

    Entity MakeEntityAt(World& world, const glm::vec3& pos, const glm::vec3& eulerDeg = {}, const glm::vec3& scale = glm::vec3(1.0f))
    {
        const Entity e = world.Create();
        world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = ComposeTransform(pos, eulerDeg, scale)});
        return e;
    }

    void CheckMatApprox(const glm::mat4& actual, const glm::mat4& expected)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                CHECK(actual[c][r] == doctest::Approx(expected[c][r]).epsilon(1e-4));
            }
        }
    }

    glm::mat4 RelativeTo(const glm::mat4& parent, const glm::mat4& child)
    {
        return glm::inverse(parent) * child;
    }
}

TEST_CASE("SetWorldTransform moves the whole subtree, preserving relative offsets") {
    World world = MakeWorld();
    const Entity parent = MakeEntityAt(world, {10.0f, 0.0f, 0.0f});
    const Entity child = MakeEntityAt(world, {12.0f, 1.0f, 0.0f}, {0.0f, 45.0f, 0.0f});
    const Entity grandchild = MakeEntityAt(world, {12.0f, 3.0f, 0.5f}, {}, glm::vec3(2.0f));
    REQUIRE(ecs::SetParent(world, child, parent));
    REQUIRE(ecs::SetParent(world, grandchild, child));

    const glm::mat4 childRel = RelativeTo(world.Get<TransformComponent>(parent).localToWorld, world.Get<TransformComponent>(child).localToWorld);
    const glm::mat4 grandRel = RelativeTo(world.Get<TransformComponent>(child).localToWorld, world.Get<TransformComponent>(grandchild).localToWorld);

    const glm::mat4 target = ComposeTransform({-5.0f, 2.0f, 8.0f}, {0.0f, 90.0f, 0.0f}, glm::vec3(1.5f));
    ecs::SetWorldTransform(world, parent, target);

    const glm::mat4& parentNow = world.Get<TransformComponent>(parent).localToWorld;
    const glm::mat4& childNow = world.Get<TransformComponent>(child).localToWorld;
    const glm::mat4& grandNow = world.Get<TransformComponent>(grandchild).localToWorld;

    CheckMatApprox(parentNow, target);
    CheckMatApprox(RelativeTo(parentNow, childNow), childRel);
    CheckMatApprox(RelativeTo(childNow, grandNow), grandRel);
}

TEST_CASE("SetWorldTransform is a pure translation delta for a translated parent") {
    World world = MakeWorld();
    const Entity parent = MakeEntityAt(world, {0.0f, 0.0f, 0.0f});
    const Entity child = MakeEntityAt(world, {3.0f, 0.0f, 0.0f});
    REQUIRE(ecs::SetParent(world, child, parent));

    glm::mat4 target = world.Get<TransformComponent>(parent).localToWorld;
    target[3] = glm::vec4(0.0f, 5.0f, 0.0f, 1.0f);
    ecs::SetWorldTransform(world, parent, target);

    CheckMatApprox(world.Get<TransformComponent>(child).localToWorld, ComposeTransform({3.0f, 5.0f, 0.0f}, {}, glm::vec3(1.0f)));
}

TEST_CASE("SetWorldTransform without a TransformComponent is a no-op") {
    World world = MakeWorld();
    const Entity parent = world.Create();
    const Entity child = MakeEntityAt(world, {1.0f, 2.0f, 3.0f});
    REQUIRE(ecs::SetParent(world, child, parent));

    const glm::mat4 before = world.Get<TransformComponent>(child).localToWorld;
    ecs::SetWorldTransform(world, parent, ComposeTransform({9.0f, 9.0f, 9.0f}, {}, glm::vec3(1.0f)));
    CheckMatApprox(world.Get<TransformComponent>(child).localToWorld, before);
}

TEST_CASE("SetWorldTransform cascades through a transformless middle link") {
    World world = MakeWorld();
    const Entity parent = MakeEntityAt(world, {0.0f, 0.0f, 0.0f});
    const Entity middle = world.Create();
    const Entity leaf = MakeEntityAt(world, {2.0f, 0.0f, 0.0f});
    REQUIRE(ecs::SetParent(world, middle, parent));
    REQUIRE(ecs::SetParent(world, leaf, middle));

    glm::mat4 target = world.Get<TransformComponent>(parent).localToWorld;
    target[3] = glm::vec4(0.0f, 0.0f, -4.0f, 1.0f);
    ecs::SetWorldTransform(world, parent, target);

    CheckMatApprox(world.Get<TransformComponent>(leaf).localToWorld, ComposeTransform({2.0f, 0.0f, -4.0f}, {}, glm::vec3(1.0f)));
}

// ── Reflected setters must be surgical, not full round-trips ─────────────────
// TransformComponent stores a matrix; position/euler/scale are DECOMPOSED views.
// A setter that decomposes and recomposes the whole matrix to change one channel
// re-injects float error into the other two - so dragging position in the
// inspector quietly degrades rotation and scale, and replicating position every
// tick makes a networked entity's scale drift with nothing to correct it.

namespace
{
	const aether::reflect::FieldDesc& TransformField(const char* name)
	{
		for (const aether::reflect::ComponentType& ct: aether::reflect::ComponentTypes())
		{
			if (ct.name != "Transform")
			{
				continue;
			}
			for (const aether::reflect::FieldDesc& f: ct.fields)
			{
				if (f.name == name)
				{
					return f;
				}
			}
		}
		FAIL("Transform field not found: " << name);
		std::abort();
	}
} // namespace

TEST_CASE("Setting position leaves rotation and scale bit-identical")
{
	aether::TransformComponent t;
	t.localToWorld = aether::ComposeTransform({1.f, 2.f, 3.f}, {30.f, 45.f, 60.f}, {2.f, 3.f, 4.f});

	const glm::mat4 before = t.localToWorld;

	aether::reflect::FieldValue v;
	v.type = aether::reflect::FieldType::Vec3;
	v.vec = {10.f, 20.f, 30.f, 0.f};
	TransformField("position").set(&t, v);

	// Translation changed exactly...
	CHECK(t.localToWorld[3].x == doctest::Approx(10.f));
	CHECK(t.localToWorld[3].y == doctest::Approx(20.f));
	CHECK(t.localToWorld[3].z == doctest::Approx(30.f));

	// ...and the three basis columns are untouched, bit for bit. Approx() would
	// hide exactly the drift this test exists to catch.
	for (int col = 0; col < 3; ++col)
	{
		CHECK(t.localToWorld[col].x == before[col].x);
		CHECK(t.localToWorld[col].y == before[col].y);
		CHECK(t.localToWorld[col].z == before[col].z);
	}
}

TEST_CASE("Repeatedly setting position does not accumulate scale drift")
{
	aether::TransformComponent t;
	t.localToWorld = aether::ComposeTransform({0.f, 0.f, 0.f}, {17.f, 133.f, -41.f}, {1.f, 1.f, 1.f});

	aether::reflect::FieldValue v;
	v.type = aether::reflect::FieldType::Vec3;

	// A networked entity applies a position snapshot every tick for minutes.
	for (int i = 0; i < 2000; ++i)
	{
		v.vec = {static_cast<float>(i) * 0.01f, 0.f, 0.f, 0.f};
		TransformField("position").set(&t, v);
	}

	const glm::vec3 scale = aether::ExtractScale(t.localToWorld);
	CHECK(scale.x == doctest::Approx(1.f).epsilon(1e-6));
	CHECK(scale.y == doctest::Approx(1.f).epsilon(1e-6));
	CHECK(scale.z == doctest::Approx(1.f).epsilon(1e-6));
}

TEST_CASE("Setting euler leaves position bit-identical")
{
	aether::TransformComponent t;
	t.localToWorld = aether::ComposeTransform({7.f, -3.f, 11.f}, {10.f, 20.f, 30.f}, {1.f, 1.f, 1.f});

	const glm::vec4 posBefore = t.localToWorld[3];

	aether::reflect::FieldValue v;
	v.type = aether::reflect::FieldType::Vec3;
	v.vec = {45.f, 45.f, 45.f, 0.f};
	TransformField("euler").set(&t, v);

	CHECK(t.localToWorld[3].x == posBefore.x);
	CHECK(t.localToWorld[3].y == posBefore.y);
	CHECK(t.localToWorld[3].z == posBefore.z);
}

// ── Viewport focus box (what F frames) ────────────────────────────────────────

TEST_CASE("Selection focus box covers every selected entity")
{
	World world;
	const Entity a = world.Create();
	const Entity b = world.Create();
	world.Emplace<TransformComponent>(a, TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(-10.0f, 0.0f, 0.0f))});
	world.Emplace<TransformComponent>(b, TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 4.0f, 0.0f))});

	const std::array<Entity, 2> both{a, b};
	const auto box = aether::editor::ComputeSelectionFocusBox(world, both);
	REQUIRE(box.has_value());
	// Centred between them, not on either one: framing only the primary was the bug.
	CHECK(box->center.x == doctest::Approx(0.0f));
	CHECK(box->center.y == doctest::Approx(2.0f));
	CHECK(box->size.x == doctest::Approx(21.0f)); // 20 apart, plus half a unit of scale each side
}

TEST_CASE("Selection focus box keeps a lone entity's framing unchanged")
{
	World world;
	const Entity only = world.Create();
	world.Emplace<TransformComponent>(only, TransformComponent{.localToWorld = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, -2.0f)), glm::vec3(4.0f))});

	const std::array<Entity, 1> one{only};
	const auto box = aether::editor::ComputeSelectionFocusBox(world, one);
	REQUIRE(box.has_value());
	CHECK(box->center.x == doctest::Approx(3.0f));
	CHECK(box->center.y == doctest::Approx(1.0f));
	CHECK(box->center.z == doctest::Approx(-2.0f));
	// The old code framed at 2.5 * max(scale); the box's largest side must still be that scale.
	CHECK(glm::max(box->size.x, glm::max(box->size.y, box->size.z)) == doctest::Approx(4.0f));
}

TEST_CASE("Selection focus box ignores entities that cannot be framed")
{
	World world;
	const Entity placed = world.Create();
	const Entity noTransform = world.Create();
	world.Emplace<TransformComponent>(placed, TransformComponent{.localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 0.0f))});

	const std::array<Entity, 3> mixed{placed, noTransform, Entity{}};
	const auto box = aether::editor::ComputeSelectionFocusBox(world, mixed);
	REQUIRE(box.has_value());
	CHECK(box->center.x == doctest::Approx(5.0f));

	const std::array<Entity, 1> nothing{noTransform};
	CHECK_FALSE(aether::editor::ComputeSelectionFocusBox(world, nothing).has_value());
}

TEST_CASE("Selection focus box measures a sprite by its quad, not its scale")
{
	World world;
	const Entity sprite = world.Create();
	world.Emplace<TransformComponent>(sprite, TransformComponent{.localToWorld = glm::mat4(1.0f)});
	// 400x200 pixels at 100 px/unit, pivot centred -> a 4 x 2 unit quad.
	world.Emplace<SpriteRendererComponent>(sprite, SpriteRendererComponent{.pixelSize = glm::vec2(400.0f, 200.0f), .pivot = glm::vec2(0.5f), .pixelsPerUnit = 100.0f});

	const std::array<Entity, 1> one{sprite};
	const auto box = aether::editor::ComputeSelectionFocusBox(world, one);
	REQUIRE(box.has_value());
	// The scale fallback would have said 1 x 1: the sprite's own size is what matters.
	CHECK(box->size.x == doctest::Approx(4.0f));
	CHECK(box->size.y == doctest::Approx(2.0f));
	CHECK(box->center.x == doctest::Approx(0.0f));
	CHECK(box->center.y == doctest::Approx(0.0f));
}

TEST_CASE("Selection focus box respects a sprite's pivot")
{
	World world;
	const Entity sprite = world.Create();
	world.Emplace<TransformComponent>(sprite, TransformComponent{.localToWorld = glm::mat4(1.0f)});
	// Pivot at the bottom-left corner, so the quad extends up and to the right of the origin.
	world.Emplace<SpriteRendererComponent>(sprite, SpriteRendererComponent{.pixelSize = glm::vec2(200.0f, 200.0f), .pivot = glm::vec2(0.0f), .pixelsPerUnit = 100.0f});

	const std::array<Entity, 1> one{sprite};
	const auto box = aether::editor::ComputeSelectionFocusBox(world, one);
	REQUIRE(box.has_value());
	CHECK(box->size.x == doctest::Approx(2.0f));
	CHECK(box->center.x == doctest::Approx(1.0f));
	CHECK(box->center.y == doctest::Approx(1.0f));
}
