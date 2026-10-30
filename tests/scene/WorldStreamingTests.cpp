#include "scene/WorldStreaming.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/SceneDocumentExpansion.hpp"
#include "scene/SceneStreamingDependencies.hpp"
#include "scene/SceneStreamingReferences.hpp"
#include "scene/World.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "utils/ServiceContainer.hpp"

#include <doctest/doctest.h>
#include <map>

using namespace aether;
using namespace aether::app::scene;

namespace
{
	struct DemandBackend final : IWorldStreamingBackend
	{
		struct Entry
		{
			streaming::Demand demand;
			io::IOPriority priority;
		};

		std::map<std::string, Entry> entries;

		Expected<void> SetBudgets(const streaming::Budgets&) override
		{
			return {};
		}

		Expected<void> SetDemand(const streaming::Region& region, streaming::Demand demand, io::IOPriority priority) override
		{
			entries.insert_or_assign(region.id, Entry{demand, priority});
			return {};
		}

		void Update() override
		{
		}

		streaming::State GetState(std::string_view) const override
		{
			return streaming::State::Loaded;
		}

		std::string GetDiagnosticsJson() const override
		{
			return "{}";
		}

		void ResetWorld() override
		{
			entries.clear();
		}

		void Shutdown() override
		{
		}
	};
} // namespace

TEST_CASE("World streaming sources apply region hysteresis independently")
{
	auto backend = std::make_unique<DemandBackend>();
	auto* observed = backend.get();
	WorldStreaming world(std::move(backend));
	streaming::Region region;
	region.id = "field";
	region.scene = "field";
	region.boundsMax = {10, 10, 10};
	region.unloadMargin = 5;
	REQUIRE(world.Configure(std::span{&region, 1}).has_value());
	streaming::Source source;
	source.id = "camera";
	source.loadRadius = 2;
	source.prefetchRadius = 4;
	source.position = {11, 0, 0};
	REQUIRE(world.SetSource(source).has_value());
	world.Update();
	CHECK(observed->entries.at("field").demand == streaming::Demand::Active);
	source.position.x = 16;
	REQUIRE(world.SetSource(source).has_value());
	world.Update();
	CHECK(observed->entries.at("field").demand == streaming::Demand::Active);
	source.position.x = 18;
	REQUIRE(world.SetSource(source).has_value());
	world.Update();
	CHECK(observed->entries.at("field").demand == streaming::Demand::Prefetch);
	CHECK(world.RemoveSource("camera"));
	world.Update();
	CHECK(observed->entries.at("field").demand == streaming::Demand::None);
}

TEST_CASE("World streaming claims are independent and cancelled requests cannot reactivate")
{
	auto backend = std::make_unique<DemandBackend>();
	auto* observed = backend.get();
	WorldStreaming world(std::move(backend));
	streaming::Region region;
	region.id = "r";
	region.scene = "r";
	REQUIRE(world.Configure(std::span{&region, 1}).has_value());
	auto prefetch = world.Request("r", false, io::IOPriority::High);
	auto active = world.Request("r", true, io::IOPriority::Critical);
	REQUIRE(prefetch != 0);
	REQUIRE(active != 0);
	world.Update();
	CHECK(observed->entries.at("r").demand == streaming::Demand::Active);
	CHECK(observed->entries.at("r").priority == io::IOPriority::Critical);
	CHECK(world.Cancel(active));
	CHECK(world.GetRequestState(active) == streaming::State::Cancelled);
	world.Update();
	CHECK(observed->entries.at("r").demand == streaming::Demand::Prefetch);
	CHECK(observed->entries.at("r").priority == io::IOPriority::High);
	CHECK(world.Release(prefetch));
	CHECK(world.Release(active));
	world.Update();
	CHECK(observed->entries.at("r").demand == streaming::Demand::None);
}

TEST_CASE("World streaming validates inputs and bounds claims")
{
	auto backend = std::make_unique<DemandBackend>();
	WorldStreaming world(std::move(backend));
	streaming::Region regions[2];
	regions[0].id = regions[1].id = "r";
	regions[0].scene = regions[1].scene = "chunk";
	CHECK_FALSE(world.Configure(regions).has_value());
	REQUIRE(world.Configure(std::span{regions, 1}).has_value());
	streaming::Source source;
	source.id = "bad";
	source.loadRadius = 10;
	source.prefetchRadius = 5;
	CHECK_FALSE(world.SetSource(source).has_value());
	streaming::Budgets budgets;
	budgets.maxPendingRequests = 1;
	REQUIRE(world.SetBudgets(budgets).has_value());
	const auto first = world.Request("r", false);
	REQUIRE(first != 0);
	CHECK(world.Request("r", true) == 0);
	CHECK(world.Cancel(first));
	CHECK(world.Request("r", true) == 0);
	CHECK(world.Release(first));
	CHECK(world.Request("r", true) != 0);
}

TEST_CASE("World streaming world reset retires claims and permits reconfiguration")
{
	auto backend = std::make_unique<DemandBackend>();
	auto* observed = backend.get();
	WorldStreaming world(std::move(backend));
	streaming::Region region;
	region.id = "r";
	region.scene = "chunk";
	REQUIRE(world.Configure(std::span{&region, 1}).has_value());
	const auto claim = world.Request("r", true);
	REQUIRE(claim != 0);
	world.Update();
	REQUIRE_FALSE(observed->entries.empty());
	world.ResetWorld();
	CHECK(observed->entries.empty());
	CHECK(world.GetRequestState(claim) == streaming::State::Unknown);
	REQUIRE(world.Configure(std::span{&region, 1}).has_value());
	CHECK(world.Request("r", false) != 0);
}

TEST_CASE("Scene binary reader rejects malformed and truncated input")
{
	SceneDescription scene;
	scene.name = "bounded";
	const auto valid = WriteSceneBinary(scene);
	REQUIRE(ReadSceneBinaryChecked(valid.data(), valid.size()).has_value());
	for (std::size_t size = 0; size < valid.size(); ++size)
	{
		CHECK_FALSE(ReadSceneBinaryChecked(valid.data(), size).has_value());
	}
	auto trailing = valid;
	trailing.push_back(std::byte{0});
	CHECK_FALSE(ReadSceneBinaryChecked(trailing.data(), trailing.size()).has_value());
	auto badRoot = valid;
	badRoot[8] = std::byte{255};
	CHECK_FALSE(ReadSceneBinaryChecked(badRoot.data(), badRoot.size()).has_value());
	auto badVersion = valid;
	badVersion[4] = std::byte{255};
	CHECK_FALSE(ReadSceneBinaryChecked(badVersion.data(), badVersion.size()).has_value());
	const std::vector<std::byte> unknownTag{std::byte{0x41},
	        std::byte{0x45},
	        std::byte{0x53},
	        std::byte{0x42},
	        std::byte{1},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{1},
	        std::byte{1},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{255}};
	CHECK_FALSE(ReadSceneBinaryChecked(unknownTag.data(), unknownTag.size()).has_value());
	auto badCount = unknownTag;
	badCount[9] = std::byte{255};
	CHECK_FALSE(ReadSceneBinaryChecked(badCount.data(), badCount.size()).has_value());
	auto badBoolean = unknownTag;
	badBoolean.back() = std::byte{6};
	badBoolean.push_back(std::byte{2});
	CHECK_FALSE(ReadSceneBinaryChecked(badBoolean.data(), badBoolean.size()).has_value());
	std::vector<std::byte> deep{std::byte{0x41},
	        std::byte{0x45},
	        std::byte{0x53},
	        std::byte{0x42},
	        std::byte{1},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{1},
	        std::byte{1},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{0},
	        std::byte{2}};
	for (unsigned i = 0; i < 65; ++i)
	{
		deep.insert(deep.end(), {std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}});
	}
	deep.insert(deep.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
	CHECK_FALSE(ReadSceneBinaryChecked(deep.data(), deep.size()).has_value());
}

TEST_CASE("Streaming script references follow stable nodes after unload and reentry")
{
	World world;
	ServiceContainer services;
	aether::app::ScriptComponentSystem scripts(services);
	const Entity owner = world.Create();
	const Entity target = world.Create();
	world.Emplace<SceneNodeComponent>(owner, SceneNodeComponent{101});
	world.Emplace<SceneNodeComponent>(target, SceneNodeComponent{202});
	ScriptPropertyValue reference;
	reference.type = ScriptPropertyValue::Type::Entity;
	reference.i64 = target.id;
	ScriptEntry entry;
	entry.path = "Probe";
	entry.properties["Target"] = reference;
	world.Emplace<ScriptComponent>(owner, ScriptComponent{{entry}});
	REQUIRE(scripts.PrepareStreamingScriptReferences(world, {target}).has_value());
	CHECK(world.Get<ScriptComponent>(owner).scripts[0].properties.at("Target").i64 == 0);
	world.Destroy(target);
	REQUIRE(scripts.ResolveStreamingScriptReferences(world).has_value());
	const Entity restored = world.Create();
	world.Emplace<SceneNodeComponent>(restored, SceneNodeComponent{202});
	REQUIRE(scripts.ResolveStreamingScriptReferences(world).has_value());
	CHECK(world.Get<ScriptComponent>(owner).scripts[0].properties.at("Target").i64 == restored.id);
}

TEST_CASE("Streaming document expansion preserves references to unloaded foreign nodes")
{
	SceneDescription root;
	root.streamingRegions.push_back(streaming::Region{.id = "foreign", .scene = "chunk"});
	EntityRecord owner;
	owner.nodeId = 101;
	ScriptRecord script;
	script.type = "Probe";
	ScriptPropertyValue target;
	target.type = ScriptPropertyValue::Type::Entity;
	target.i64 = -1;
	script.properties.emplace("Target", target);
	script.nodeRefs.emplace("Target", 202);
	owner.scripts.push_back(std::move(script));
	root.entities.push_back(std::move(owner));
	const SceneDocumentReader noDocuments = [](std::string_view, SceneDocumentKind) -> std::expected<SceneDescription, std::string>
	{
		return std::unexpected("Unexpected document read");
	};
	const auto expanded = ExpandSceneDocument(root, noDocuments);
	REQUIRE(expanded.has_value());
	CHECK(expanded->entities[0].scripts[0].nodeRefs.at("Target") == 202);
	CHECK(expanded->entities[0].scripts[0].properties.at("Target").i64 == -1);
	root.streamingRegions.clear();
	CHECK_FALSE(ExpandSceneDocument(root, noDocuments).has_value());
}

TEST_CASE("Streamed source aliases defer missing nodes and reject ambiguous instances")
{
	World world;
	const auto absent = ResolveSceneNodeReference(world, 202);
	REQUIRE(absent.has_value());
	CHECK_FALSE(absent->IsValid());
	const Entity first = world.Create();
	const auto firstNode = StreamingNodeId("first", 202);
	world.Emplace<SceneNodeComponent>(first, SceneNodeComponent{.id = firstNode, .sourceId = 202});
	auto resolved = ResolveSceneNodeReference(world, 202);
	REQUIRE(resolved.has_value());
	CHECK(resolved->id == first.id);
	const Entity second = world.Create();
	const auto secondNode = StreamingNodeId("second", 202);
	REQUIRE(firstNode != secondNode);
	world.Emplace<SceneNodeComponent>(second, SceneNodeComponent{.id = secondNode, .sourceId = 202});
	CHECK_FALSE(ResolveSceneNodeReference(world, 202).has_value());
	resolved = ResolveSceneNodeReference(world, firstNode);
	REQUIRE(resolved.has_value());
	CHECK(resolved->id == first.id);
	world.Destroy(first);
	resolved = ResolveSceneNodeReference(world, 202);
	REQUIRE(resolved.has_value());
	CHECK(resolved->id == second.id);
}

TEST_CASE("Canonical streamed references do not switch to another instance after unload")
{
	World world;
	ServiceContainer services;
	aether::app::ScriptComponentSystem scripts(services);
	const Entity owner = world.Create();
	const Entity target = world.Create();
	const auto targetNode = StreamingNodeId("first", 202);
	world.Emplace<SceneNodeComponent>(owner, SceneNodeComponent{101});
	world.Emplace<SceneNodeComponent>(target, SceneNodeComponent{.id = targetNode, .sourceId = 202});
	ScriptPropertyValue reference;
	reference.type = ScriptPropertyValue::Type::Entity;
	SUBCASE("Entity property")
	{
	}
	SUBCASE("Component property")
	{
		reference.type = ScriptPropertyValue::Type::Component;
		reference.str = "Transform";
	}
	reference.i64 = target.id;
	ScriptEntry entry;
	entry.path = "Probe";
	entry.properties["Target"] = reference;
	world.Emplace<ScriptComponent>(owner, ScriptComponent{{entry}});
	REQUIRE(scripts.PrepareStreamingScriptReferences(world, {target}).has_value());
	world.Destroy(target);
	const Entity other = world.Create();
	world.Emplace<SceneNodeComponent>(other, SceneNodeComponent{.id = StreamingNodeId("second", 202), .sourceId = 202});
	REQUIRE(scripts.ResolveStreamingScriptReferences(world).has_value());
	CHECK(world.Get<ScriptComponent>(owner).scripts[0].properties.at("Target").i64 == 0);
	const Entity restored = world.Create();
	world.Emplace<SceneNodeComponent>(restored, SceneNodeComponent{.id = targetNode, .sourceId = 202});
	REQUIRE(scripts.ResolveStreamingScriptReferences(world).has_value());
	CHECK(world.Get<ScriptComponent>(owner).scripts[0].properties.at("Target").i64 == restored.id);
}

TEST_CASE("Nested streamed field references retain canonical identities across missing targets")
{
	World world;
	const Entity original = world.Create();
	const auto canonical = StreamingNodeId("first", 202);
	world.Emplace<SceneNodeComponent>(original, SceneNodeComponent{.id = canonical, .sourceId = 202});
	reflect::FieldValue target;
	target.type = reflect::FieldType::EntityRef;
	target.entity = 202;
	reflect::FieldValue stable;
	stable.type = reflect::FieldType::List;
	stable.list = {{target}};
	auto applied = stable;
	bool missing = false;
	REQUIRE(ResolveStreamingFieldValue(stable, applied, StreamingNodeIndex(world), missing).has_value());
	CHECK_FALSE(missing);
	CHECK(applied.list[0][0].entity == original.id);
	CHECK(stable.list[0][0].entity == canonical);
	world.Destroy(original);
	const Entity other = world.Create();
	world.Emplace<SceneNodeComponent>(other, SceneNodeComponent{.id = StreamingNodeId("second", 202), .sourceId = 202});
	applied = stable;
	REQUIRE(ResolveStreamingFieldValue(stable, applied, StreamingNodeIndex(world), missing).has_value());
	CHECK(missing);
	CHECK(applied.list[0][0].entity == 0);
	CHECK(stable.list[0][0].entity == canonical);
	const Entity restored = world.Create();
	world.Emplace<SceneNodeComponent>(restored, SceneNodeComponent{.id = canonical, .sourceId = 202});
	applied = stable;
	missing = false;
	REQUIRE(ResolveStreamingFieldValue(stable, applied, StreamingNodeIndex(world), missing).has_value());
	CHECK_FALSE(missing);
	CHECK(applied.list[0][0].entity == restored.id);
}
