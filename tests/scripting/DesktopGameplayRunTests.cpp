#include <doctest/doctest.h>

#include "scripting/GameplayRun.hpp"
#include "scripting/GameplayHost.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "scene/World.hpp"
#include "io/FileUtil.hpp"
#include "utils/ServiceContainer.hpp"
#include <chrono>

using namespace aether::app::scripting;

TEST_CASE("Desktop gameplay launch configuration requires a known bounded profile")
{
	GameplayRunRequest request;
	request.launchConfig = {{"profile", "native-desktop-2d-v1"}};
	CHECK_NOTHROW(ValidateGameplayRunConfig(request));
	request.launchConfig["profile"] = "future-browser";
	CHECK_THROWS(ValidateGameplayRunConfig(request));
	request.launchConfig["profile"] = "native-desktop-2d-v1";
	request.ticks = 0;
	CHECK_THROWS(ValidateGameplayRunConfig(request));
}

TEST_CASE("Desktop checkpoints reject repeated ticks and cannot silently run headlessly")
{
	GameplayRunRequest request;
	const nlohmann::json assertion = {{"entity", "Pickup"}, {"type", "$engine"}, {"property", "exists"}, {"equals", false}};
	request.launchConfig = {{"profile", "native-desktop-2d-v1"}, {"checkpoints", {{{"tick", 10}, {"assertions", {assertion}}}}}};
	CHECK_NOTHROW(ValidateGameplayRunConfig(request));
	request.launchConfig["checkpoints"].push_back(request.launchConfig["checkpoints"][0]);
	CHECK_THROWS(ValidateGameplayRunConfig(request));
	request.launchConfig["checkpoints"].erase(1);
	request.launchConfig.erase("profile");
	CHECK_THROWS(ValidateGameplayRunConfig(request));
}

TEST_CASE("Gameplay observations distinguish a missing entity from duplicate names")
{
	aether::World world;
	aether::ServiceContainer services;
	aether::app::ScriptComponentSystem scripts(services);
	GameplayHost host;
	GameplayRunRequest request;
	request.output = std::filesystem::temp_directory_path() / ("aether-observation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
	request.launchConfig["assertions"] = {{{"entity", "Pickup"}, {"type", "$engine"}, {"property", "exists"}, {"equals", false}}};
	CHECK_NOTHROW(WriteGameplayState(request, world, host, scripts, false));
	for (int i = 0; i < 2; ++i)
	{
		const auto entity = world.Create();
		world.Emplace<aether::NameComponent>(entity, aether::NameComponent{.name = "Pickup"});
	}
	request.launchConfig["assertions"][0]["equals"] = true;
	CHECK_THROWS(WriteGameplayState(request, world, host, scripts, false));
	const auto artifact = aether::io::file_util::ReadText(request.output);
	REQUIRE(artifact);
	CHECK(nlohmann::json::parse(*artifact)["assertions"][0]["passed"] == false);
	std::error_code error;
	std::filesystem::remove(request.output, error);
	CHECK_FALSE(error);
}
