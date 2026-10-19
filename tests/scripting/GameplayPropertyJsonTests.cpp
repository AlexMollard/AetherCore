#include <doctest/doctest.h>

#include <limits>
#include "scripting/GameplayPropertyJson.hpp"

using namespace aether;
using namespace aether::app::scripting;
using nlohmann::json;

TEST_CASE("Gameplay JSON uses Rust declarations for entity float enum and typed component fields")
{
	const ScriptPropertyInfo fields[] = {{"counter", ScriptPropertyValue::Type::Entity}, {"speed", ScriptPropertyValue::Type::Float}, {"mode", ScriptPropertyValue::Type::Enum}, {"body", ScriptPropertyValue::Type::Component, {}, "Rigid Body 2D"}};
	auto result = ParseGameplayScriptProperties(json{{"counter", 2}, {"speed", 3}, {"mode", 1}, {"body", 2}}, fields);
	REQUIRE(result);
	CHECK(result->at("counter").type == ScriptPropertyValue::Type::Entity);
	CHECK(result->at("counter").i64 == 2);
	CHECK(result->at("speed").type == ScriptPropertyValue::Type::Float);
	CHECK(result->at("speed").f4[0] == 3.0f);
	CHECK(result->at("mode").type == ScriptPropertyValue::Type::Enum);
	CHECK(result->at("body").type == ScriptPropertyValue::Type::Component);
	CHECK(result->at("body").str == "Rigid Body 2D");
	result = ParseGameplayScriptProperties(json{{"counter", 0}, {"body", {{"entity", 0}, {"component", "Rigid Body 2D"}}}}, fields);
	REQUIRE(result);
	CHECK(result->at("counter").i64 == 0);
	CHECK(result->at("body").i64 == 0);
	CHECK(result->at("body").str == "Rigid Body 2D");
	result = ParseGameplayScriptProperties(json{{"counter", uint64_t{0xFFFFFFFF}}}, fields);
	REQUIRE(result);
	CHECK(result->at("counter").i64 == 0xFFFFFFFF);
}

TEST_CASE("Gameplay JSON rejects wrong Rust shapes and numeric ranges")
{
	const ScriptPropertyInfo fields[] = {{"counter", ScriptPropertyValue::Type::Entity},
	        {"count", ScriptPropertyValue::Type::Int},
	        {"speed", ScriptPropertyValue::Type::Float},
	        {"mode", ScriptPropertyValue::Type::Enum},
	        {"body", ScriptPropertyValue::Type::Component, {}, "Rigid Body 2D"},
	        {"vector", ScriptPropertyValue::Type::Vector3},
	        {"enabled", ScriptPropertyValue::Type::Bool}};
	for (const auto& input: {json{{"counter", -1}},
	             json{{"counter", 4294967296ULL}},
	             json{{"counter", 2.5}},
	             json{{"count", 2.0}},
	             json{{"mode", 1.0}},
	             json{{"body", 1.0}},
	             json{{"body", {{"entity", 2}, {"component", "Collider 2D"}}}},
	             json{{"speed", "3"}},
	             json{{"speed", std::numeric_limits<double>::infinity()}},
	             json{{"speed", std::numeric_limits<double>::max()}},
	             json{{"vector", {1, "bad", 3}}},
	             json{{"enabled", 1}},
	             json{{"missing", 1}}})
	{
		CHECK_FALSE(ParseGameplayScriptProperties(input, fields));
	}
}

TEST_CASE("Gameplay JSON preserves UTF8 NUL vector bool and legacy CSharp inference")
{
	const std::string text = std::string("h\xC3\xA9\0llo", 7);
	const ScriptPropertyInfo fields[] = {{"label", ScriptPropertyValue::Type::String}, {"offset", ScriptPropertyValue::Type::Vector3}, {"enabled", ScriptPropertyValue::Type::Bool}};
	const auto declared = ParseGameplayScriptProperties(json{{"label", text}, {"offset", {1, 2.5, 3}}, {"enabled", true}}, fields);
	REQUIRE(declared);
	CHECK(declared->at("label").str == text);
	CHECK(declared->at("offset").f4[1] == 2.5f);
	CHECK(declared->at("enabled").i64 == 1);
	const auto legacy = ParseGameplayScriptProperties(json{{"counter", 2}, {"speed", 2.0}, {"label", text}, {"offset", {1, 2, 3}}, {"enabled", false}});
	REQUIRE(legacy);
	CHECK(legacy->at("counter").type == ScriptPropertyValue::Type::Int);
	CHECK(legacy->at("speed").type == ScriptPropertyValue::Type::Float);
	CHECK(legacy->at("label").str == text);
	CHECK_FALSE(ParseGameplayScriptProperties(json{{"invalid", json::object()}}));
	CHECK_FALSE(ParseGameplayScriptProperties(json::array()));
}

TEST_CASE("Gameplay JSON strict metadata rejects fields on a zero-property Rust script")
{
	CHECK_FALSE(ParseGameplayScriptProperties(json{{"unknown", 2}}, {}, true));
	const auto empty = ParseGameplayScriptProperties(json::object(), {}, true);
	REQUIRE(empty);
	CHECK(empty->empty());
	const auto legacy = ParseGameplayScriptProperties(json{{"unknown", 2}}, {}, false);
	REQUIRE(legacy);
	CHECK(legacy->at("unknown").type == ScriptPropertyValue::Type::Int);
}
