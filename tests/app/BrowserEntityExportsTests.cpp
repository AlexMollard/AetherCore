#include <doctest/doctest.h>

#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "scripting/interop/InteropCommon.hpp"
#include "web/BrowserMaterial.hpp"

extern "C"
{
	std::int32_t aether_mesh_renderer_get_visible(std::uint32_t id);
	void aether_mesh_renderer_set_visible(std::uint32_t id, std::int32_t visible);
}

namespace browser_entity_fixture
{
	aether::World* s_world = nullptr;
#include "../../src/app/web/BrowserEntityExports.inc"
#include "../../src/app/web/BrowserMaterialExports.inc"
}

TEST_CASE("Browser material edits preserve per-entity values and hand texture paths to the draw resolver")
{
	using namespace aether;
	using namespace browser_entity_fixture;
	World world;
	s_world = &world;
	const auto entity = world.Create();
	MaterialAsset material;
	material.baseColorFactor = {1.0f, 1.0f, 1.0f, 0.4f};
	material.roughnessFactor = 0.7f;
	material.normalTex = TextureHandle{8, 1};
	world.Emplace<MaterialInstanceComponent>(entity, MaterialInstanceComponent{material});
	aether_entity_material_set_color(entity.id, {0.2f, 0.3f, 0.4f});
	CHECK(world.Get<MaterialInstanceComponent>(entity).asset.baseColorFactor == glm::vec4(0.2f, 0.3f, 0.4f, 0.4f));
	CHECK(world.Get<MaterialInstanceComponent>(entity).asset.roughnessFactor == 0.7f);
	aether_set_material_texture(entity.id, "project://skin.png");
	const auto* texture = world.TryGet<app::web::BrowserMaterialTexture>(entity);
	REQUIRE(texture);
	CHECK(texture->path == "project://skin.png");
	unsigned resolves = 0;
	auto resolve = [&](std::string_view path) -> Expected<TextureHandle>
	{
		++resolves;
		CHECK(path == "project://skin.png");
		return TextureHandle{9, 1};
	};
	REQUIRE(app::web::ResolveBrowserMaterialTexture(world, entity, resolve));
	CHECK(resolves == 1);
	CHECK(world.Get<MaterialInstanceComponent>(entity).asset.albedoTex == TextureHandle{9, 1});
	CHECK(world.Get<MaterialInstanceComponent>(entity).asset.normalTex == material.normalTex);
	aether_set_material_texture(entity.id, "project://skin.png");
	REQUIRE(app::web::ResolveBrowserMaterialTexture(world, entity, resolve));
	CHECK(resolves == 1);
	aether_set_material_texture(entity.id, nullptr);
	REQUIRE(app::web::ResolveBrowserMaterialTexture(world, entity, resolve));
	CHECK_FALSE(world.Get<MaterialInstanceComponent>(entity).asset.albedoTex.IsValid());
	CHECK(resolves == 1);
	world.Destroy(entity);
	aether_entity_material_set_color(entity.id, {});
	aether_set_material_texture(entity.id, "project://invalid.png");
	CHECK_FALSE(world.TryGet<app::web::BrowserMaterialTexture>(entity));
	s_world = nullptr;
	aether_entity_material_set_color(entity.id, {});
	aether_set_material_texture(entity.id, "project://invalid.png");
}

TEST_CASE("Browser SDK hierarchy exports enumerate children and reject invalid handles and indices")
{
	using namespace aether;
	using namespace browser_entity_fixture;
	World world;
	s_world = &world;
	struct ResetWorld
	{
		~ResetWorld() { browser_entity_fixture::s_world = nullptr; }
	} reset;
	const auto parent = world.Create();
	const auto first = world.Create();
	const auto second = world.Create();
	aether_entity_set_parent(first.id, parent.id);
	aether_entity_set_parent(second.id, parent.id);
	CHECK(aether_entity_child_count(parent.id) == 2);
	CHECK(aether_entity_child_at(parent.id, 0) == first.id);
	CHECK(aether_entity_child_at(parent.id, 1) == second.id);
	CHECK(aether_entity_get_parent(first.id) == parent.id);
	CHECK(aether_entity_child_at(parent.id, -1) == 0);
	CHECK(aether_entity_child_at(parent.id, 2) == 0);
	CHECK(aether_entity_child_at(parent.id, INT32_MAX) == 0);
	CHECK(aether_entity_child_count(first.id) == 0);
	CHECK(aether_entity_child_at(0, 0) == 0);
	aether_entity_set_parent(parent.id, first.id);
	CHECK(aether_entity_get_parent(parent.id) == 0);
	aether_entity_set_parent(first.id, 0);
	CHECK(aether_entity_get_parent(first.id) == 0);
	CHECK(aether_entity_child_count(parent.id) == 1);
	CHECK(aether_entity_child_at(parent.id, 0) == second.id);
	world.Destroy(first);
	aether_entity_set_parent(second.id, first.id);
	aether_entity_set_parent(first.id, parent.id);
	CHECK(aether_entity_get_parent(second.id) == parent.id);
	CHECK(aether_entity_child_count(first.id) == 0);
	s_world = nullptr;
	CHECK(aether_entity_child_count(parent.id) == 0);
	CHECK(aether_entity_get_parent(second.id) == 0);
	CHECK(aether_entity_child_at(parent.id, 0) == 0);
	aether_entity_set_parent(second.id, parent.id);
}

TEST_CASE("Browser shared mesh renderer exports toggle visibility without creating missing renderers")
{
	using namespace aether;
	World world;
	app::scripting::SceneContext context{.world = &world};
	const app::scripting::ActiveContextScope scope(context);
	const auto visible = world.Create();
	world.Emplace<MeshRendererComponent>(visible);
	CHECK(aether_mesh_renderer_get_visible(visible.id) == 1);
	aether_mesh_renderer_set_visible(visible.id, 0);
	CHECK_FALSE(world.Get<MeshRendererComponent>(visible).visible);
	CHECK(aether_mesh_renderer_get_visible(visible.id) == 0);
	aether_mesh_renderer_set_visible(visible.id, -1);
	CHECK(world.Get<MeshRendererComponent>(visible).visible);
	const auto missing = world.Create();
	aether_mesh_renderer_set_visible(missing.id, 1);
	CHECK_FALSE(world.Has<MeshRendererComponent>(missing));
	CHECK(aether_mesh_renderer_get_visible(missing.id) == 0);
	world.Destroy(visible);
	aether_mesh_renderer_set_visible(visible.id, 1);
	CHECK(aether_mesh_renderer_get_visible(visible.id) == 0);
}
