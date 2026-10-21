#include "scene/SceneDocumentExpansion.hpp"

#include <doctest/doctest.h>

#include <map>

using namespace aether;
using namespace aether::app::scene;

namespace
{
	EntityRecord ExpansionEntity(std::string name, int parent = -1, float x = 0.0f)
	{
		EntityRecord entity;
		entity.name = std::move(name);
		entity.parentIndex = parent;
		entity.hasTransform = true;
		entity.position.x = x;
		return entity;
	}

	ScriptRecord ExpansionReference(std::int64_t index, std::uint64_t node = 0)
	{
		ScriptRecord script;
		script.type = "Test";
		ScriptPropertyValue value;
		value.type = ScriptPropertyValue::Type::Entity;
		value.i64 = index;
		script.properties["target"] = value;
		if (node != 0)
		{
			script.nodeRefs["target"] = node;
		}
		return script;
	}

	SceneDocumentReader ExpansionReader(const std::map<std::string, SceneDescription>& documents)
	{
		return [&documents](std::string_view path, SceneDocumentKind) -> std::expected<SceneDescription, std::string>
		{
			const auto found = documents.find(std::string(path));
			if (found == documents.end())
			{
				return std::unexpected("missing test document");
			}
			return found->second;
		};
	}
} // namespace

TEST_CASE("SceneDocumentExpansion includes preserve host environment and remap file-local references")
{
	SceneDescription host;
	host.name = "host";
	host.environment.emplace();
	host.environment->sunIntensity = 4.0f;
	host.entities.push_back(ExpansionEntity("host"));
	host.includes = {"included", "included"};
	SceneDescription included;
	included.environment.emplace();
	included.environment->sunIntensity = 9.0f;
	included.entities = {ExpansionEntity("parent"), ExpansionEntity("child", 0)};
	included.entities[1].scripts.push_back(ExpansionReference(0));
	included.entities[1].joint.emplace();
	included.entities[1].joint->targetIndex = 0;
	std::map<std::string, SceneDescription> documents{{"included", included}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	REQUIRE(result->entities.size() == 3);
	CHECK(result->entities[2].parentIndex == 1);
	CHECK(result->entities[2].scripts[0].properties.at("target").i64 == 1);
	CHECK(result->entities[2].joint->targetIndex == 1);
	CHECK(result->environment->sunIntensity == 4.0f);
	CHECK(result->includes.empty());
}

TEST_CASE("SceneDocumentExpansion rejects future dependency versions")
{
	SceneDescription host;
	host.includes = {"future"};
	SceneDescription future;
	future.version = kSceneFormatVersion + 1;
	const std::map<std::string, SceneDescription> documents{{"future", future}};
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents)));
	host.includes.clear();
	host.prefabInstances.push_back(PrefabInstanceRecord{.prefabPath = "future"});
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents)));
}

TEST_CASE("SceneDocumentExpansion prefab placement uses source root delta with world-space children")
{
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("root", -1, 2.0f), ExpansionEntity("child", 0, 3.0f), ExpansionEntity("other", -1, 30.0f)};
	SceneDescription host;
	PrefabInstanceRecord instance;
	instance.prefabPath = "prefab";
	instance.position.x = 10.0f;
	instance.scale = glm::vec3(2.0f);
	instance.name = "placed";
	host.prefabInstances.push_back(instance);
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	REQUIRE(result->entities.size() == 3);
	CHECK(result->entities[0].name == "placed");
	CHECK(result->entities[0].position.x == doctest::Approx(10.0f));
	CHECK(result->entities[1].position.x == doctest::Approx(12.0f));
	CHECK(result->entities[2].position.x == doctest::Approx(30.0f));
	CHECK(result->prefabInstances.empty());
}

TEST_CASE("SceneDocumentExpansion GUID overrides preserve instance root and source component fields")
{
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("root", -1, 2.0f), ExpansionEntity("child", 0, 3.0f)};
	prefab.entities[0].guid = 90;
	prefab.entities[1].guid = 20;
	prefab.entities[1].tags = {"kept"};
	SceneDescription host;
	PrefabInstanceRecord instance;
	instance.prefabPath = "prefab";
	instance.position.x = 10.0f;
	instance.name = "placed";
	instance.overrides = {{20, "name = 'edited'"}, {90, "name = 'ignored'\nposition = [50.0, 0.0, 0.0]\ndisabled = true"}};
	host.prefabInstances.push_back(instance);
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	CHECK(result->entities[0].name == "placed");
	CHECK(result->entities[0].position.x == doctest::Approx(10.0f));
	CHECK(result->entities[0].disabled);
	CHECK(result->entities[1].name == "edited");
	CHECK(result->entities[1].position.x == doctest::Approx(3.0f));
	CHECK(result->entities[1].tags == std::vector<std::string>{"kept"});
	CHECK(result->entities[1].parentIndex == 0);
}

TEST_CASE("SceneDocumentExpansion removals cascade and compact references while additions keep world pose")
{
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("root"), ExpansionEntity("removed", 0), ExpansionEntity("removed-child", 1), ExpansionEntity("kept", 0)};
	prefab.entities[0].scripts = {ExpansionReference(1), ExpansionReference(3)};
	SceneDescription host;
	PrefabInstanceRecord instance;
	instance.prefabPath = "prefab";
	instance.removedGuids = {1, 2}; // removing the instance root is ignored by native apply
	instance.position.x = 10.0f;
	instance.addedEntities = {ExpansionEntity("added", -1, 50.0f), ExpansionEntity("added-child", 0, 60.0f)};
	instance.addedEntities[1].scripts = {ExpansionReference(0)};
	host.prefabInstances.push_back(instance);
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	REQUIRE(result->entities.size() == 4);
	CHECK(result->entities[0].scripts[0].properties.at("target").i64 == -1);
	CHECK(result->entities[0].scripts[1].properties.at("target").i64 == 1);
	CHECK(result->entities[2].name == "added");
	CHECK(result->entities[2].position.x == 50.0f);
	CHECK(result->entities[2].parentIndex == 0);
	CHECK(result->entities[3].parentIndex == 2);
	CHECK(result->entities[3].scripts[0].properties.at("target").i64 == 2);
}

TEST_CASE("SceneDocumentExpansion resolves script scene nodes across includes and prefab instances")
{
	SceneDescription host;
	host.entities = {ExpansionEntity("host")};
	host.entities[0].scripts = {ExpansionReference(-1, 200)};
	PrefabInstanceRecord instance;
	instance.prefabPath = "prefab";
	instance.node = 200;
	host.prefabInstances = {instance};
	host.includes = {"included"};
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("prefab-root")};
	prefab.entities[0].scripts = {ExpansionReference(-1, 300)};
	SceneDescription included;
	included.entities = {ExpansionEntity("included")};
	included.entities[0].nodeId = 300;
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}, {"included", included}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	CHECK(result->entities[0].scripts[0].properties.at("target").i64 == 1);
	CHECK(result->entities[1].scripts[0].properties.at("target").i64 == 2);
	CHECK(result->entities[1].scripts[0].nodeRefs.empty());
}

TEST_CASE("SceneDocumentExpansion rejects missing dependencies cycles depth and entity overflow")
{
	SceneDescription host;
	host.name = "host";
	host.includes = {"a"};
	std::map<std::string, SceneDescription> documents;
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents)).has_value());
	documents["a"].includes = {"host"};
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents)).has_value());
	documents["a"].includes.clear();
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents), {.maxDepth = 0}).has_value());
	documents["a"].entities = {ExpansionEntity("one"), ExpansionEntity("two")};
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents), {.maxEntities = 1}).has_value());
	host.includes.clear();
	host.prefabInstances.emplace_back().prefabPath = "a";
	documents["a"].prefabInstances.emplace_back().prefabPath = "a";
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents)).has_value());
}

TEST_CASE("SceneDocumentExpansion nested instances retain independent world placement and script index spaces")
{
	SceneDescription inner;
	inner.entities = {ExpansionEntity("inner", -1, 1.0f), ExpansionEntity("inner-child", 0, 2.0f)};
	inner.entities[1].scripts = {ExpansionReference(0)};
	SceneDescription outer;
	outer.entities = {ExpansionEntity("outer")};
	PrefabInstanceRecord nested;
	nested.prefabPath = "inner";
	nested.position.x = 20.0f;
	outer.prefabInstances = {nested};
	SceneDescription host;
	host.entities = {ExpansionEntity("host")};
	PrefabInstanceRecord placed;
	placed.prefabPath = "outer";
	placed.position.x = 100.0f;
	host.prefabInstances = {placed};
	std::map<std::string, SceneDescription> documents{{"inner", inner}, {"outer", outer}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	REQUIRE(result->entities.size() == 4);
	CHECK(result->entities[1].position.x == doctest::Approx(100.0f));
	CHECK(result->entities[2].position.x == doctest::Approx(20.0f));
	CHECK(result->entities[2].parentIndex == -1);
	CHECK(result->entities[3].position.x == doctest::Approx(21.0f));
	CHECK(result->entities[3].parentIndex == 2);
	CHECK(result->entities[3].scripts[0].properties.at("target").i64 == 2);
}

TEST_CASE("SceneDocumentExpansion prefab override scripts rebind to the full prefab index space")
{
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("root"), ExpansionEntity("child", 0)};
	EntityRecord overridden = prefab.entities[0];
	overridden.scripts = {ExpansionReference(1)};
	PrefabInstanceRecord instance;
	instance.prefabPath = "prefab";
	instance.overrides = {{1, ComputePrefabOverrideToml(overridden, prefab.entities[0])}};
	SceneDescription host;
	host.entities = {ExpansionEntity("host")};
	host.prefabInstances = {instance};
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	REQUIRE(result->entities[1].scripts.size() == 1);
	CHECK(result->entities[1].scripts[0].properties.at("target").i64 == 2);
}

TEST_CASE("SceneDocumentExpansion root GUID overrides preserve the placed instance scene-node identity")
{
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("source-root")};
	prefab.entities[0].guid = 70;
	prefab.entities[0].nodeId = 100;
	PrefabInstanceRecord instance;
	instance.prefabPath = "prefab";
	instance.node = 200;
	instance.overrides = {{70, "disabled = true"}};
	SceneDescription host;
	host.entities = {ExpansionEntity("external-holder")};
	host.entities[0].scripts = {ExpansionReference(-1, 200)};
	host.prefabInstances = {instance};
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}};
	const auto result = ExpandSceneDocument(host, ExpansionReader(documents));
	REQUIRE(result.has_value());
	REQUIRE(result->entities.size() == 2);
	CHECK(result->entities[1].nodeId == 200);
	CHECK(result->entities[1].disabled);
	CHECK(result->entities[0].scripts[0].properties.at("target").i64 == 1);
	CHECK(result->entities[0].scripts[0].nodeRefs.empty());
}

TEST_CASE("SceneDocumentExpansion rejects ambiguous malformed hierarchies overrides and script nodes")
{
	SceneDescription host;
	host.entities = {ExpansionEntity("self", 0)};
	CHECK_FALSE(ExpandSceneDocument(host, {}).has_value());
	host.entities[0].parentIndex = 8;
	CHECK_FALSE(ExpandSceneDocument(host, {}).has_value());
	host.entities[0].parentIndex = -1;
	host.entities[0].scripts = {ExpansionReference(-1, 300)};
	CHECK_FALSE(ExpandSceneDocument(host, {}).has_value());
	host.entities.push_back(ExpansionEntity("duplicate"));
	host.entities[0].nodeId = 300;
	host.entities[1].nodeId = 300;
	CHECK_FALSE(ExpandSceneDocument(host, {}).has_value());
	host.entities.clear();
	host.prefabInstances.emplace_back().prefabPath = "prefab";
	host.prefabInstances[0].overrides = {{1, "name = ["}};
	SceneDescription prefab;
	prefab.entities = {ExpansionEntity("root")};
	std::map<std::string, SceneDescription> documents{{"prefab", prefab}};
	CHECK_FALSE(ExpandSceneDocument(host, ExpansionReader(documents)).has_value());
}
