// Native reference for the browser 3D physics parity fixture: the same authored scene
// (resources/web/reference/scenes/BrowserPhysics3D.scene.toml) is applied through the native
// SceneSerializer and stepped at the fixed 1/60 tick. The browser runs the same PhysicsSystem
// compiled to WebAssembly (single-threaded, same Jolt revision, CROSS_PLATFORM_DETERMINISTIC),
// so the positions below must agree with aetherTest.entities() at the same tick.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
#include "scene/Components.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/World.hpp"

using namespace aether;

TEST_CASE("Physics3D browser parity fixture: native reference positions")
{
	const auto path = std::filesystem::path(AETHER_TESTS_SOURCE_DIR).parent_path() / "resources/web/reference/scenes/BrowserPhysics3D.scene.toml";
	std::ifstream stream(path);
	REQUIRE(stream.good());
	std::stringstream text;
	text << stream.rdbuf();
	const auto parsed = app::scene::ParseToml(text.str());
	REQUIRE(parsed.has_value());

	World world;
	world.SetSceneKind(parsed->kind);
	world.SetSceneFeatures(parsed->features);
	auto system = std::make_unique<PhysicsSystem>();
	PhysicsSystem* physics = system.get();
	world.RegisterSystem(std::move(system));
	const auto created = app::scene::ApplyScene(*parsed, world, app::scene::ApplySceneDeps{});
	REQUIRE(created.size() == parsed->entities.size());

	std::ostringstream report;
	report.precision(9);
	int tick = 0;
	for (const int checkpoint: {1, 30, 60})
	{
		for (; tick < checkpoint; ++tick)
		{
			world.UpdateSystems(1.0f / 60.0f);
		}
		report << "tick " << checkpoint << "\n";
		for (std::size_t i = 0; i < created.size(); ++i)
		{
			const auto& m = world.Get<TransformComponent>(created[i]).localToWorld;
			report << "  " << parsed->entities[i].name << " " << m[3].x << " " << m[3].y << " " << m[3].z << "\n";
		}
	}
	std::ofstream out(std::filesystem::path(AETHER_TESTS_SOURCE_DIR).parent_path() / "logs/task5/native-physics3d.txt");
	out << report.str();
	(void) physics;
	MESSAGE(report.str());
}
