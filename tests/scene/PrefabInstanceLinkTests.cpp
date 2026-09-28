// Linked prefab instances as the building block of an area scene (logs/prefabs/DESIGN.md
// §2.10 Phase 0): an instance ROOT's own edits persist (E1), script Entity references
// between instances persist by scene-node id (E3), and a scene can include other scenes
// additively without ever saving their content itself (E4).

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "material/MaterialRegistry.hpp"
#include "material/TextureRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/TagSlots.hpp"
#include "scene/TransformUtils.hpp"
#include "scene/World.hpp"
#include "../material/FakeSlotSink.hpp"
#include "../material/FakeTextureSink.hpp"

using namespace aether;
using namespace aether::app::scene;

namespace
{
	ScriptPropertyValue IntProp(std::int64_t v)
	{
		ScriptPropertyValue p;
		p.type = ScriptPropertyValue::Type::Int;
		p.i64 = v;
		return p;
	}

	ScriptPropertyValue EntityProp(Entity e)
	{
		ScriptPropertyValue p;
		p.type = ScriptPropertyValue::Type::Entity;
		p.i64 = e.id;
		return p;
	}

	Entity FindNamed(World& world, std::string_view name)
	{
		Entity found{};
		world.View<NameComponent>().each(
		        [&](entt::entity h, const NameComponent& n)
		        {
			        if (n.name == name)
			        {
				        found = World::FromEntt(h);
			        }
		        });
		return found;
	}

	int CountNamed(World& world, std::string_view name)
	{
		int n = 0;
		world.View<NameComponent>().each([&](entt::entity, const NameComponent& c) { n += c.name == name ? 1 : 0; });
		return n;
	}

	std::int64_t Prop(World& world, Entity e, const std::string& name)
	{
		const auto* sc = world.TryGet<ScriptComponent>(e);
		REQUIRE(sc != nullptr);
		REQUIRE(!sc->scripts.empty());
		const auto it = sc->scripts[0].properties.find(name);
		REQUIRE(it != sc->scripts[0].properties.end());
		return it->second.i64;
	}

	void SetProp(World& world, Entity e, const std::string& name, const ScriptPropertyValue& v)
	{
		world.Get<ScriptComponent>(e).scripts[0].properties[name] = v;
	}

	std::uint64_t NodeOf(World& world, Entity e)
	{
		const auto* n = world.TryGet<SceneNodeComponent>(e);
		return n != nullptr ? n->id : 0;
	}

	Entity AddScripted(World& world, const std::string& name, const std::string& script)
	{
		const Entity e = world.Create();
		world.Emplace<NameComponent>(e, NameComponent{.name = name});
		world.Emplace<TransformComponent>(e, TransformComponent{});
		world.Emplace<ScriptComponent>(e, ScriptComponent{.scripts = {ScriptEntry{.path = script, .attached = false}}});
		return e;
	}

	// Project-scoped scene + prefab folders under a fresh temp dir, plus the registries every
	// capture needs. Removed (and the scoping cleared) on destruction.
	struct LinkFixture
	{
		std::filesystem::path dir;
		FakeSlotSink sink{8};
		FakeTextureSink tsink;
		TextureRegistry treg{tsink};
		MaterialRegistry mreg{sink, treg};

		explicit LinkFixture(std::string_view name)
		        : dir(std::filesystem::temp_directory_path() / name)
		{
			std::filesystem::remove_all(dir);
			std::filesystem::create_directories(dir / "scenes");
			std::filesystem::create_directories(dir / "prefabs");
			SetProjectSceneDirectories(dir / "scenes", dir / "prefabs");
		}

		~LinkFixture()
		{
			ClearProjectSceneDirectories();
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}

		// A descriptor-carrying prefab like the Twinsanity ones: a root with a data script
		// (Layer, Link0) and one child body.
		SceneDescription SaveLinker(const std::string& prefabName, const std::string& childName = "Body")
		{
			World authoring;
			const Entity root = AddScripted(authoring, "Linker", "TwObject");
			SetProp(authoring, root, "Layer", IntProp(0));
			SetProp(authoring, root, "Link0", EntityProp(Entity{}));
			const Entity body = authoring.Create();
			authoring.Emplace<NameComponent>(body, NameComponent{.name = childName});
			authoring.Emplace<TransformComponent>(body, TransformComponent{.localToWorld = ComposeTransform({0, 0.5f, 0}, {0, 0, 0}, {1, 1, 1})});
			ecs::SetParent(authoring, body, root);
			REQUIRE(SavePrefabFile(prefabName, CapturePrefab(authoring, root, mreg, treg)));
			const auto saved = ReadPrefabFile(prefabName);
			REQUIRE(saved.has_value());
			return *saved;
		}
	};

	Entity FindInstance(World& world, std::string_view name)
	{
		const Entity e = FindNamed(world, name);
		REQUIRE(e.IsValid());
		REQUIRE(world.Has<PrefabInstanceComponent>(e));
		return e;
	}

	const std::vector<Entity>& ChildrenOf(World& world, Entity e)
	{
		static const std::vector<Entity> kNone;
		const auto* h = world.TryGet<HierarchyComponent>(e);
		return h != nullptr ? h->children : kNone;
	}
} // namespace

TEST_CASE("E1: an instance root's script and tag edits survive save/load and a prefab re-save") {
	LinkFixture fx("aether_prefab_link_e1");
	const SceneDescription prefab = fx.SaveLinker("tw_test_e1");

	World live;
	const Entity a = InstantiatePrefabInstance("tw_test_e1", prefab, live, ApplySceneDeps{}, ComposeTransform({5, 0, 2}, {0, 45, 0}, {1, 1, 1}));
	REQUIRE(a.IsValid());
	live.Get<NameComponent>(a).name = "act_A";
	SetProp(live, a, "Layer", IntProp(7));
	TagAdd(&live, a.id, TagCreate("tw_test_e1_tag"));

	const SceneDescription captured = CaptureScene(live, fx.mreg, fx.treg);
	REQUIRE(captured.prefabInstances.size() == 1);
	const PrefabInstanceRecord& rec = captured.prefabInstances[0];
	REQUIRE(rec.overrides.size() == 1);
	CHECK(rec.overrides[0].guid == EffectiveGuid(prefab.entities[0], 0));
	// Transform and name are the instance record's own fields, never a root override.
	CHECK(rec.overrides[0].partialToml.find("position") == std::string::npos);
	CHECK(rec.overrides[0].partialToml.find("name") == std::string::npos);
	CHECK(rec.name == "act_A");
	REQUIRE(SaveSceneFile("E1Scene", captured));

	World loaded;
	REQUIRE(LoadSceneFile("E1Scene", loaded, ApplySceneDeps{}));
	const Entity root = FindInstance(loaded, "act_A");
	CHECK(Prop(loaded, root, "Layer") == 7);
	CHECK(TagHas(&loaded, root.id, TagGetId("tw_test_e1_tag")));
	// The root's children survive the root override being re-applied.
	REQUIRE(ChildrenOf(loaded, root).size() == 1);
	const Entity body = ChildrenOf(loaded, root)[0];
	CHECK(loaded.Get<NameComponent>(body).name == "Body");
	CHECK(loaded.Get<HierarchyComponent>(body).parent == root);
	glm::vec3 pos{}, euler{}, scale{};
	DecomposeTRS(loaded.Get<TransformComponent>(root).localToWorld, pos, euler, scale);
	CHECK(pos.x == doctest::Approx(5.0f));
	CHECK(pos.z == doctest::Approx(2.0f));
	CHECK(euler.y == doctest::Approx(45.0f));

	// Re-capturing the loaded scene reproduces the same override (no churn on re-save).
	const SceneDescription again = CaptureScene(loaded, fx.mreg, fx.treg);
	REQUIRE(again.prefabInstances.size() == 1);
	REQUIRE(again.prefabInstances[0].overrides.size() == 1);
	CHECK(again.prefabInstances[0].overrides[0].partialToml == rec.overrides[0].partialToml);
	CHECK(again.prefabInstances[0].removedGuids.empty());
	CHECK(again.prefabInstances[0].addedEntities.empty());

	// Re-save the prefab with a changed child: the child change reaches the instance, the
	// root's per-instance data stays.
	fx.SaveLinker("tw_test_e1", "Hull");
	World reloaded;
	REQUIRE(LoadSceneFile("E1Scene", reloaded, ApplySceneDeps{}));
	const Entity root2 = FindInstance(reloaded, "act_A");
	CHECK(Prop(reloaded, root2, "Layer") == 7);
	REQUIRE(ChildrenOf(reloaded, root2).size() == 1);
	CHECK(reloaded.Get<NameComponent>(ChildrenOf(reloaded, root2)[0]).name == "Hull");
}

TEST_CASE("E3: entity refs to instance roots survive save/load, reordering and prefab re-expansion") {
	LinkFixture fx("aether_prefab_link_e3");
	const SceneDescription prefab = fx.SaveLinker("tw_test_e3");

	World live;
	const Entity a = InstantiatePrefabInstance("tw_test_e3", prefab, live, ApplySceneDeps{}, ComposeTransform({1, 0, 0}, {0, 0, 0}, {1, 1, 1}), 111);
	const Entity b = InstantiatePrefabInstance("tw_test_e3", prefab, live, ApplySceneDeps{}, ComposeTransform({9, 0, 0}, {0, 0, 0}, {1, 1, 1}), 222);
	live.Get<NameComponent>(a).name = "act_A";
	live.Get<NameComponent>(b).name = "act_B";
	CHECK(NodeOf(live, a) == 111);
	CHECK(NodeOf(live, b) == 222);
	SetProp(live, a, "Link0", EntityProp(b)); // instance root -> instance root (a root override)
	const Entity holder = AddScripted(live, "Holder", "TwTrigger");
	SetProp(live, holder, "Target0", EntityProp(b)); // top-level scene entity -> instance root
	const Entity point = AddScripted(live, "Point 0", "TwPoint");
	SetProp(live, point, "Owner", EntityProp(b)); // an instance's added entity -> another instance
	ecs::SetParent(live, point, a);

	const SceneDescription captured = CaptureScene(live, fx.mreg, fx.treg);
	const std::string text = WriteToml(captured);
	CHECK(text.find("v_node = 222") != std::string::npos);

	auto parsed = ParseToml(text);
	REQUIRE(parsed.has_value());
	REQUIRE(parsed->prefabInstances.size() == 2);
	// Reorder: instances swapped, an unrelated entity inserted ahead of the holder, and the
	// target world already holds other entities, so no index or entity id lines up by accident.
	std::reverse(parsed->prefabInstances.begin(), parsed->prefabInstances.end());
	EntityRecord dummy;
	dummy.name = "Dummy";
	parsed->entities.insert(parsed->entities.begin(), dummy);
	World loaded;
	for (int i = 0; i < 5; ++i)
	{
		static_cast<void>(loaded.Create());
	}
	ReplaceScene(*parsed, loaded, ApplySceneDeps{});

	const Entity a2 = FindInstance(loaded, "act_A");
	const Entity b2 = FindInstance(loaded, "act_B");
	CHECK(NodeOf(loaded, a2) == 111);
	CHECK(NodeOf(loaded, b2) == 222);
	CHECK(Prop(loaded, a2, "Link0") == b2.id);
	CHECK(Prop(loaded, FindNamed(loaded, "Holder"), "Target0") == b2.id);
	const Entity point2 = FindNamed(loaded, "Point 0");
	REQUIRE(point2.IsValid());
	CHECK(loaded.Get<HierarchyComponent>(point2).parent == a2);
	CHECK(Prop(loaded, point2, "Owner") == b2.id);

	// Instances save in node-id order, not in whatever order the load expanded them (reversed here).
	const SceneDescription recaptured = CaptureScene(loaded, fx.mreg, fx.treg);
	REQUIRE(recaptured.prefabInstances.size() == 2);
	CHECK(recaptured.prefabInstances[0].node == 111);
	CHECK(recaptured.prefabInstances[1].node == 222);

	// The cooked binary carries the same node ids.
	const auto binary = ReadSceneBinary(WriteSceneBinary(captured));
	REQUIRE(binary.has_value());
	REQUIRE(binary->prefabInstances.size() == 2);
	CHECK(binary->prefabInstances[0].node + binary->prefabInstances[1].node == 333);

	// Prefab re-expansion: applying B to the prefab rebuilds A, whose refs resolve again.
	REQUIRE(ApplyPrefabInstanceToPrefab(loaded, b2, ApplySceneDeps{}, fx.mreg, fx.treg));
	const Entity a3 = FindInstance(loaded, "act_A");
	CHECK(NodeOf(loaded, a3) == 111);
	CHECK(Prop(loaded, a3, "Link0") == b2.id);
	CHECK(Prop(loaded, FindNamed(loaded, "Point 0"), "Owner") == b2.id);

	// Applying A rebuilds B: every ref held outside B follows it to its new root.
	REQUIRE(ApplyPrefabInstanceToPrefab(loaded, a3, ApplySceneDeps{}, fx.mreg, fx.treg));
	const Entity b3 = FindInstance(loaded, "act_B");
	CHECK(NodeOf(loaded, b3) == 222);
	CHECK(Prop(loaded, a3, "Link0") == b3.id);
	CHECK(Prop(loaded, FindNamed(loaded, "Holder"), "Target0") == b3.id);
	// A scene node reference never becomes part of the prefab itself.
	const auto resaved = ReadPrefabFile("tw_test_e3");
	REQUIRE(resaved.has_value());
	CHECK(resaved->entities[0].scripts[0].nodeRefs.empty());
}

TEST_CASE("E4: an include applies, is not re-saved into the host, and survives Play snapshot/restore") {
	LinkFixture fx("aether_prefab_link_e4");
	const SceneDescription prefab = fx.SaveLinker("tw_test_e4");

	// The area scene: a flat entity with a child, an instance, and a ref to that instance.
	{
		World area;
		const Entity rock = area.Create();
		area.Emplace<NameComponent>(rock, NameComponent{.name = "Rock"});
		area.Emplace<TransformComponent>(rock, TransformComponent{});
		const Entity pebble = area.Create();
		area.Emplace<NameComponent>(pebble, NameComponent{.name = "Pebble"});
		area.Emplace<TransformComponent>(pebble, TransformComponent{});
		ecs::SetParent(area, pebble, rock);
		const Entity c = InstantiatePrefabInstance("tw_test_e4", prefab, area, ApplySceneDeps{}, glm::mat4(1.0f), 333);
		area.Get<NameComponent>(c).name = "act_C";
		const Entity trigger = AddScripted(area, "Trigger", "TwTrigger");
		SetProp(area, trigger, "Target0", EntityProp(c));
		REQUIRE(SaveSceneFile("E4Area", CaptureScene(area, fx.mreg, fx.treg)));
	}
	{
		World host;
		const Entity level = host.Create();
		host.Emplace<NameComponent>(level, NameComponent{.name = "Level"});
		host.Emplace<TransformComponent>(level, TransformComponent{});
		SceneDescription desc = CaptureScene(host, fx.mreg, fx.treg);
		desc.name = "E4World";
		desc.includes = {"E4Area", "E4Area"}; // a duplicate is applied once
		REQUIRE(SaveSceneFile("E4World", desc));
	}

	World world;
	REQUIRE(LoadSceneFile("E4World", world, ApplySceneDeps{}));
	const auto checkIncluded = [&world]
	{
		CHECK(CountNamed(world, "Rock") == 1);
		CHECK(CountNamed(world, "act_C") == 1);
		const Entity rock = FindNamed(world, "Rock");
		REQUIRE(rock.IsValid());
		CHECK(world.Has<SceneTransientComponent>(rock));
		REQUIRE(world.Has<IncludedFromComponent>(rock));
		CHECK(world.Get<IncludedFromComponent>(rock).scene == "E4Area");
		CHECK(world.Get<HierarchyComponent>(FindNamed(world, "Pebble")).parent == rock);
		const Entity c = FindInstance(world, "act_C");
		CHECK(world.Has<IncludedFromComponent>(c));
		CHECK(NodeOf(world, c) == 333);
		CHECK(Prop(world, FindNamed(world, "Trigger"), "Target0") == c.id);
	};
	checkIncluded();

	// The host captures its own content plus the include list - never the included entities.
	const SceneDescription snapshot = CaptureScene(world, fx.mreg, fx.treg);
	REQUIRE(snapshot.entities.size() == 1);
	CHECK(snapshot.entities[0].name == "Level");
	CHECK(snapshot.prefabInstances.empty());
	CHECK(snapshot.includes == std::vector<std::string>{"E4Area", "E4Area"});
	const std::string text = WriteToml(snapshot);
	CHECK(text.find("[[includes]]") != std::string::npos);
	CHECK(text.find("Rock") == std::string::npos);

	// Play: the session changes the included content; Stop restores the snapshot.
	ecs::DestroyHierarchy(world, FindNamed(world, "Rock"));
	RestoreSceneInPlace(snapshot, world, ApplySceneDeps{});
	checkIncluded();
	CHECK(CountNamed(world, "Level") == 1);
	CHECK(CaptureScene(world, fx.mreg, fx.treg).includes == snapshot.includes);

	// Loading a scene without includes drops the include list.
	ReplaceScene(SceneDescription{}, world, ApplySceneDeps{});
	CHECK(CaptureScene(world, fx.mreg, fx.treg).includes.empty());
}

TEST_CASE("A load then save of a yawed instance adds no child transform override") {
	// Converted crates sit at arbitrary yaws far from the origin, and their static crate body
	// child's pose comes back from the physics side with float noise in the last digits
	// (euler 14.787592 under a 14.787594 root, scale 0.99999976). Noise is not an edit and
	// must not freeze as a per-child override.
	LinkFixture fx("aether_prefab_link_noise");
	const SceneDescription prefab = fx.SaveLinker("tw_test_noise");
	World live;
	for (int i = 0; i < 16; ++i)
	{
		const float yaw = 14.787594f + 23.1f * static_cast<float>(i);
		const Entity e = InstantiatePrefabInstance("tw_test_noise", prefab, live, ApplySceneDeps{}, ComposeTransform({312.4f + static_cast<float>(i), 5.25f, -87.3f}, {0, yaw, 0}, {1, 1, 1}), 500 + static_cast<std::uint64_t>(i));
		REQUIRE(e.IsValid());
	}
	REQUIRE(SaveSceneFile("NoiseScene", CaptureScene(live, fx.mreg, fx.treg)));
	World loaded;
	REQUIRE(LoadSceneFile("NoiseScene", loaded, ApplySceneDeps{}));
	loaded.View<PrefabInstanceComponent>().each(
	        [&](entt::entity handle, const PrefabInstanceComponent&)
	        {
		        for (const Entity child: ChildrenOf(loaded, World::FromEntt(handle)))
		        {
			        auto& tc = loaded.Get<TransformComponent>(child);
			        glm::vec3 p{}, r{}, s{};
			        DecomposeTRS(tc.localToWorld, p, r, s);
			        tc.localToWorld = ComposeTransform(p + glm::vec3(3e-6f, 0, -2e-6f), r + glm::vec3(0, -2e-6f, 0), s * glm::vec3(0.99999976f, 1, 0.99999976f));
		        }
	        });
	const SceneDescription resaved = CaptureScene(loaded, fx.mreg, fx.treg);
	REQUIRE(resaved.prefabInstances.size() == 16);
	for (const PrefabInstanceRecord& rec: resaved.prefabInstances)
	{
		CHECK(rec.overrides.empty());
	}

	// A real edit is still an override.
	const Entity first = FindInstance(loaded, "Linker");
	auto& moved = loaded.Get<TransformComponent>(ChildrenOf(loaded, first)[0]);
	moved.localToWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.01f, 0)) * moved.localToWorld;
	std::size_t overridden = 0;
	for (const PrefabInstanceRecord& rec: CaptureScene(loaded, fx.mreg, fx.treg).prefabInstances)
	{
		overridden += rec.overrides.size();
	}
	CHECK(overridden == 1);
}

TEST_CASE("An unedited instance of a prefab saved before a component field existed has no override") {
	LinkFixture fx("aether_prefab_link_old_field");
	// Written before Darkness Volume had fade_depth: the table exists, the key does not, so the
	// component loads with the reflected default and the live capture writes that default.
	{
		std::ofstream out(fx.dir / "prefabs" / "tw_test_old.prefab.toml");
		out << "[[entities]]\nguid = 1\nname = 'Old'\nparent = -1\nposition = [ 0.0, 0.0, 0.0 ]\neuler = [ 0.0, 0.0, 0.0 ]\nscale = [ 1.0, 1.0, 1.0 ]\n\n"
		       "    [entities.darkness_volume]\n";
	}
	const auto prefab = ReadPrefabFile("tw_test_old");
	REQUIRE(prefab.has_value());
	World live;
	const Entity inst = InstantiatePrefabInstance("tw_test_old", *prefab, live, ApplySceneDeps{}, ComposeTransform({4, 1, -3}, {0, 30, 0}, {2, 3, 4}));
	REQUIRE(inst.IsValid());
	REQUIRE(live.Has<DarknessVolumeComponent>(inst));
	const SceneDescription captured = CaptureScene(live, fx.mreg, fx.treg);
	REQUIRE(captured.prefabInstances.size() == 1);
	CHECK(captured.prefabInstances[0].overrides.empty());

	// A value that differs from the default is still an edit.
	live.Get<DarknessVolumeComponent>(inst).fadeDepth = 9.0f;
	const SceneDescription edited = CaptureScene(live, fx.mreg, fx.treg);
	REQUIRE(edited.prefabInstances[0].overrides.size() == 1);
	CHECK(edited.prefabInstances[0].overrides[0].partialToml.find("fade_depth") != std::string::npos);
}

TEST_CASE("Applying a placed instance keeps the prefab at the origin and its root's own script values") {
	LinkFixture fx("aether_prefab_link_apply_placement");
	const SceneDescription prefab = fx.SaveLinker("tw_test_apply");
	World live;
	const Entity inst = InstantiatePrefabInstance("tw_test_apply", prefab, live, ApplySceneDeps{}, ComposeTransform({50, 7, -20}, {0, 45, 0}, {1, 1, 1}));
	REQUIRE(inst.IsValid());
	live.Get<NameComponent>(inst).name = "act_Placed";
	SetProp(live, inst, "Layer", IntProp(7)); // per-instance data
	const Entity body = ChildrenOf(live, inst)[0];
	live.Get<NameComponent>(body).name = "Hull"; // a real content edit

	REQUIRE(ApplyPrefabInstanceToPrefab(live, inst, ApplySceneDeps{}, fx.mreg, fx.treg));
	const auto saved = ReadPrefabFile("tw_test_apply");
	REQUIRE(saved.has_value());
	REQUIRE(saved->entities.size() == 2);
	const EntityRecord& root = saved->entities[0];
	CHECK(root.name == "Linker");
	CHECK(glm::length(root.position) == doctest::Approx(0.0f));
	CHECK(glm::length(root.eulerDeg) == doctest::Approx(0.0f));
	REQUIRE(!root.scripts.empty());
	CHECK(root.scripts[0].properties.at("Layer").i64 == 0);
	const EntityRecord& child = saved->entities[1];
	CHECK(child.name == "Hull");
	CHECK(child.position.x == doctest::Approx(0.0f).epsilon(1e-4));
	CHECK(child.position.y == doctest::Approx(0.5f));
	CHECK(child.position.z == doctest::Approx(0.0f).epsilon(1e-4));
}
