#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "editor/ControlDiagnostics.hpp"

using aether::editor::DescribeUnknownMethod;
using aether::editor::JsonMatchesDeclaredType;
using aether::editor::JsonTypeName;
using aether::editor::MethodIdentity;
using nlohmann::json;

namespace
{
	// A slice of the real table: two crowded namespaces, one leaf name that also exists
	// elsewhere, and the alias/name split that dispatch does not bridge.
	const std::vector<MethodIdentity>& Methods()
	{
		static const std::vector<MethodIdentity> methods = {
		        {"scene.entities", "list_entities"},
		        {"scene.get", "get_entity"},
		        {"scene.add_component", "add_component"},
		        {"scene.lights", "list_lights"},
		        {"scene.load", "load_scene"},
		        {"settings.get", "get_setting"},
		        {"settings.set", "set_setting"},
		        {"assets.list", "list_assets"},
		        {"render.stats", "render_stats"},
		        {"ui.scroll", "ui_scroll"},
		        {"viewport.screenshot", "screenshot"},
		};
		return methods;
	}
} // namespace

TEST_CASE("DescribeUnknownMethod suggests a near miss the caller can act on")
{
	// entity/entities and scrol/scroll are the shape a caller actually produces, and
	// neither is a subsequence of the name it was reaching for - a subsequence matcher
	// alone finds nothing here.
	const std::string entity = DescribeUnknownMethod("scene.entity", Methods());
	CHECK(entity.find("scene.entities") != std::string::npos);

	const std::string scroll = DescribeUnknownMethod("ui.scrol", Methods());
	CHECK(scroll.find("ui.scroll") != std::string::npos);

	const std::string stats = DescribeUnknownMethod("render.stat", Methods());
	CHECK(stats.find("render.stats") != std::string::npos);
}

TEST_CASE("DescribeUnknownMethod finds a leaf that lives in another namespace")
{
	// The caller guessed the wrong namespace, not the wrong verb.
	const std::string shot = DescribeUnknownMethod("editor.screenshot", Methods());
	CHECK(shot.find("viewport.screenshot") != std::string::npos);
}

TEST_CASE("DescribeUnknownMethod names the method when the MCP alias was used")
{
	const std::string aliased = DescribeUnknownMethod("list_entities", Methods());
	CHECK(aliased.find("scene.entities") != std::string::npos);
	CHECK(aliased.find("alias") != std::string::npos);
}

TEST_CASE("DescribeUnknownMethod does not treat a shared namespace as a suggestion")
{
	// Nothing in settings. resembles "list", so the reply must not rank the namespace
	// and print its members - that is noise wearing the shape of an answer. It should
	// still say the namespace exists and how big it is.
	const std::string listed = DescribeUnknownMethod("settings.list", Methods());
	CHECK(listed.find("settings.get") == std::string::npos);
	CHECK(listed.find("settings.set") == std::string::npos);
	CHECK(listed.find("'settings.' namespace has 2 methods") != std::string::npos);
	// And two shared leading letters is not a resemblance: "li" alone drags in lights,
	// line and list, which is how a suggestion list stops being worth reading.
	CHECK(listed.find("scene.lights") == std::string::npos);
	// A genuine leaf match elsewhere is still worth offering.
	CHECK(listed.find("assets.list") != std::string::npos);
}

TEST_CASE("DescribeUnknownMethod invents nothing for a name unlike anything")
{
	const std::string nonsense = DescribeUnknownMethod("totally_bogus", Methods());
	CHECK(nonsense.find("Closest:") == std::string::npos);
	CHECK(nonsense.find("describe") != std::string::npos);
}

TEST_CASE("JsonTypeName spells the type as a refusal should")
{
	CHECK(JsonTypeName(json("x")) == "string");
	CHECK(JsonTypeName(json(true)) == "boolean");
	CHECK(JsonTypeName(json(3)) == "integer");
	CHECK(JsonTypeName(json(3.5)) == "number");
	CHECK(JsonTypeName(json::array()) == "array");
	CHECK(JsonTypeName(json::object()) == "object");
	CHECK(JsonTypeName(json()) == "null");
}

TEST_CASE("JsonMatchesDeclaredType accepts what a handler can read and refuses what it cannot")
{
	CHECK(JsonMatchesDeclaredType(json(3), "integer"));
	CHECK(JsonMatchesDeclaredType(json(3), "number"));   // an int is a number
	CHECK_FALSE(JsonMatchesDeclaredType(json(3.5), "integer")); // but 3.5 is not an int
	CHECK(JsonMatchesDeclaredType(json("x"), "string"));
	CHECK_FALSE(JsonMatchesDeclaredType(json("x"), "number"));
	CHECK_FALSE(JsonMatchesDeclaredType(json("3"), "integer")); // a quoted number is a string
	CHECK(JsonMatchesDeclaredType(json(true), "boolean"));
	CHECK_FALSE(JsonMatchesDeclaredType(json(1), "boolean")); // 1 is not true
	CHECK(JsonMatchesDeclaredType(json::array({1, 2, 3}), "array"));
	CHECK_FALSE(JsonMatchesDeclaredType(json("here"), "array"));
	CHECK(JsonMatchesDeclaredType(json::object(), "object"));

	// A type the schema does not model, or does not declare, is the handler's business.
	CHECK(JsonMatchesDeclaredType(json("anything"), "someFutureType"));
	CHECK(JsonMatchesDeclaredType(json(7), ""));
}
