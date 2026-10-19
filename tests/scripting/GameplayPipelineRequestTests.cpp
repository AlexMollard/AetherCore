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
