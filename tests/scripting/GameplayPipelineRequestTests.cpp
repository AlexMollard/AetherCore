#include <doctest/doctest.h>

#include "scripting/GameplayPipelineRequest.hpp"

using aether::app::scripting::ValidateGameplayPipelineRequest;

TEST_CASE("Gameplay pipeline rejects invalid operations targets and bounded launch arguments")
{
	using Json = nlohmann::json;
	const Json valid{{"project", "path with spaces & café"}, {"operation", "test"}};
	CHECK(ValidateGameplayPipelineRequest(valid));
	for (const Json& invalid: std::vector<Json>{Json{}, Json::array(), Json{{"operation", "test"}}, Json{{"project", ""}, {"operation", "test"}},
	        Json{{"project", "p"}, {"operation", "unknown"}}, Json{{"project", "p"}, {"operation", "test"}, {"target", "webgpu"}}})
	{
		CHECK_FALSE(ValidateGameplayPipelineRequest(invalid));
	}
	for (const auto& [field, value]: Json{{"ticks", -1}, {"step", 0}, {"timeoutMs", 0}, {"launchConfig", "shell command"}, {"surprise", true}}.items())
	{
		auto invalid = valid;
		invalid[field] = value;
		CHECK_FALSE(ValidateGameplayPipelineRequest(invalid));
	}
}

TEST_CASE("Gameplay pipeline preserves literal project paths and normalizes explicit launch policy")
{
	const auto parsed = ValidateGameplayPipelineRequest({{"project", "p café & $HOME"}, {"operation", "relaunch"}, {"launchConfig", {{"readSaves", true}}}});
	REQUIRE(parsed);
	CHECK(parsed->at("project") == "p café & $HOME");
	CHECK(parsed->value("target", "") == "desktop");
	CHECK(parsed->value("ticks", 0) == 60);
	CHECK(parsed->at("launchConfig").at("readSaves") == true);
}

TEST_CASE("Gameplay Web publishing preserves the explicit target and bounds operations")
{
    const auto web = ValidateGameplayPipelineRequest({{"project", "p"}, {"operation", "publish"}, {"target", "web"}});
    REQUIRE(web);
    CHECK(web->at("target") == "web");
    CHECK_FALSE(ValidateGameplayPipelineRequest({{"project", "p"}, {"operation", "run"}, {"target", "web"}}));
}

TEST_CASE("Gameplay test suites normalize bounded named cases without changing single requests")
{
	using Json = nlohmann::json;
	const Json config{{"assertions", Json::array({{{"entity", "Player"}, {"type", "$engine"}, {"property", "exists"}, {"equals", true}}})}};
	const Json request{{"project", "p"}, {"operation", "test"}, {"scene", "Start"}, {"ticks", 90}, {"launchConfig", config}, {"cases", {{{"name", "first"}}, {{"name", "caf\xc3\xa9 & branch"}, {"ticks", 10}}}}};
	const auto parsed = ValidateGameplayPipelineRequest(request);
	REQUIRE(parsed);
	CHECK(parsed->at("cases")[0]["scene"] == "Start");
	CHECK(parsed->at("cases")[0]["ticks"] == 90);
	CHECK(parsed->at("cases")[1]["ticks"] == 10);
	CHECK(parsed->at("cases")[1]["launchConfig"] == config);
	CHECK(ValidateGameplayPipelineRequest(*parsed));
	for (const auto& cases: std::vector<Json>{Json::array(),
	             Json::object(),
	             Json::array({{{"name", "same"}}, {{"name", "same"}}}),
	             Json::array({{{"name", " "}}}),
	             Json::array({{{"name", std::string(129, 'x')}}}),
	             Json::array({{{"name", "x"}, {"module", "untrusted"}}}),
	             Json::array({{{"name", "x"}, {"ticks", 0}}})})
	{
		auto invalid = request;
		invalid["cases"] = cases;
		CHECK_FALSE(ValidateGameplayPipelineRequest(invalid));
	}
	for (const auto* operation: {"build", "run", "publish", "inspect"})
	{
		auto invalid = request;
		invalid["operation"] = operation;
		CHECK_FALSE(ValidateGameplayPipelineRequest(invalid));
	}
	auto oversized = request;
	oversized["cases"] = Json::array();
	for (int index = 0; index < 33; ++index)
	{
		oversized["cases"].push_back({{"name", std::to_string(index)}});
	}
	CHECK_FALSE(ValidateGameplayPipelineRequest(oversized));
}
