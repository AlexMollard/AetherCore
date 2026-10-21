#include <doctest/doctest.h>

#include "scripting/GameplayRun.hpp"
#include "scripting/GameplayHost.hpp"
#include "systems/ScriptComponentSystem.hpp"
#include "scene/World.hpp"
#include "io/FileUtil.hpp"
#include "utils/ServiceContainer.hpp"
#include <chrono>
#include <limits>

using namespace aether::app::scripting;

TEST_CASE("Desktop render scale override is ephemeral, finite and profile bounded")
{
	GameplayRunRequest request;
	request.launchConfig = {{"profile", "native-desktop-2d-v1"}, {"renderScale", 0.5}};
	CHECK_NOTHROW(ValidateGameplayRunConfig(request));
	for (const auto value: {0.25, 1.0, 2.0})
	{
		request.launchConfig["renderScale"] = value;
		CHECK_NOTHROW(ValidateGameplayRunConfig(request));
	}
	for (const nlohmann::json& value: {nlohmann::json(false), nlohmann::json("0.5"), nlohmann::json(nullptr), nlohmann::json(0.249), nlohmann::json(2.01), nlohmann::json(std::numeric_limits<double>::infinity())})
	{
		request.launchConfig["renderScale"] = value;
		CHECK_THROWS(ValidateGameplayRunConfig(request));
	}
	request.launchConfig = {{"profile", "native-headless-2d-v1"}, {"renderScale", 0.5}};
	CHECK_THROWS(ValidateGameplayRunConfig(request));
}

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
	CHECK_THROWS_WITH(WriteGameplayState(request, world, host, scripts, false), doctest::Contains("GAMEPLAY_ASSERTION_AMBIGUOUS_TARGET"));
	const auto artifact = aether::io::file_util::ReadText(request.output);
	REQUIRE(artifact);
	CHECK(nlohmann::json::parse(*artifact)["assertions"][0]["passed"] == false);
	std::error_code error;
	std::filesystem::remove(request.output, error);
	CHECK_FALSE(error);
}

TEST_CASE("Gameplay assertion feedback publishes all failures and exact snapshot context")
{
	using Json = nlohmann::json;
	aether::World world;
	aether::ServiceContainer services;
	aether::app::ScriptComponentSystem scripts(services);
	GameplayHost host;
	GameplayRunRequest request;
	request.ticks = 10;
	request.output = std::filesystem::temp_directory_path() / ("aether-feedback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
	const auto assertion = [](const char* entity, const char* type, const char* property, Json expected)
	{
		return Json{{"entity", entity}, {"type", type}, {"property", property}, {"equals", expected}};
	};
	request.launchConfig["assertions"] = {assertion("Player", "Probe", "score", 9007199254740993LL),
	        assertion("Absent", "Probe", "score", 1),
	        assertion("Player", "UnknownType", "score", 1),
	        assertion("Player", "Probe", "missing", nullptr),
	        assertion("Duplicate", "Probe", "score", 1),
	        assertion("Player", "Probe", "label", "caf\xc3\xa9\n\"agent\""),
	        assertion("Player", "Probe", "nullable", nullptr),
	        assertion("Absent", "$engine", "exists", false)};
	const Json observations = {{{"entity", "Player"}, {"type", "Probe"}, {"properties", {{"score", 9007199254740992LL}, {"label", "different"}, {"nullable", nullptr}}}},
	        {{"entity", "Duplicate"}, {"type", "Probe"}, {"properties", {{"score", 1}}}},
	        {{"entity", "Duplicate"}, {"type", "Probe"}, {"properties", {{"score", 2}}}}};
	aether::scripting::AgentReport report{.jobId = "feedback", .status = aether::scripting::AgentJobStatus::Failed, .phase = "run", .target = "desktop", .language = "rust", .profile = "native-desktop-2d-v1"};
	CHECK_THROWS(WriteGameplayState(request, world, host, scripts, false, observations, &report));
	REQUIRE(report.diagnostics.size() == 6);
	CHECK(report.diagnostics[0].code == "GAMEPLAY_ASSERTION_VALUE_MISMATCH");
	CHECK(report.diagnostics[1].code == "GAMEPLAY_ASSERTION_MISSING_ENTITY");
	CHECK(report.diagnostics[2].code == "GAMEPLAY_ASSERTION_MISSING_TYPE");
	CHECK(report.diagnostics[3].code == "GAMEPLAY_ASSERTION_MISSING_PROPERTY");
	CHECK(report.diagnostics[4].code == "GAMEPLAY_ASSERTION_AMBIGUOUS_TARGET");
	const auto detail = Json::parse(report.diagnostics[0].message);
	CHECK(detail["tick"] == 10);
	CHECK(detail["assertionIndex"] == 0);
	CHECK(detail["expected"] == 9007199254740993LL);
	CHECK(detail["actual"] == 9007199254740992LL);
	CHECK(detail["actualPresent"] == true);
	CHECK(Json::parse(report.diagnostics[3].message)["actualPresent"] == false);
	CHECK(Json::parse(report.diagnostics[4].message)["matchCount"] == 2);
	CHECK(Json::parse(report.diagnostics[4].message)["actualPresent"] == false);
	CHECK(Json::parse(report.diagnostics[5].message)["expected"] == request.launchConfig["assertions"][5]["equals"]);
	REQUIRE(report.artifacts.size() == 1);
	CHECK(std::filesystem::u8path(report.artifacts[0].path) == request.output);
	CHECK(report.artifacts[0].bytes == std::filesystem::file_size(request.output));
	REQUIRE(report.diagnostics[0].source);
	CHECK(report.diagnostics[0].source->file == report.artifacts[0].path);
	CHECK(aether::scripting::SerializeAgentReport(report).has_value());
	report.diagnostics.clear();
	report.artifacts.clear();
	request.launchConfig["assertions"] = {assertion("Player", "Probe", "nullable", nullptr), assertion("Removed", "$engine", "exists", false), assertion("Player", "Probe", "score", 9007199254740992LL)};
	CHECK_NOTHROW(WriteGameplayState(request, world, host, scripts, false, observations, &report));
	CHECK(report.diagnostics.empty());
	REQUIRE(report.artifacts.size() == 1);
	CHECK(report.artifacts[0].bytes == std::filesystem::file_size(request.output));
	report.artifacts.clear();
	request.launchConfig["assertions"] = Json::array({assertion("Absent", "$engine", "exists", true)});
	CHECK_THROWS_WITH(WriteGameplayState(request, world, host, scripts, false, observations, &report), doctest::Contains("GAMEPLAY_ASSERTION_VALUE_MISMATCH"));
	REQUIRE(report.diagnostics.size() == 1);
	CHECK(report.diagnostics[0].code == "GAMEPLAY_ASSERTION_VALUE_MISMATCH");
	CHECK(Json::parse(report.diagnostics[0].message)["actual"] == false);
	CHECK(Json::parse(report.diagnostics[0].message)["actualPresent"] == true);
	std::filesystem::remove(request.output);
	report.artifacts.clear();
	report.diagnostics.clear();
	// A failed write must never advertise an unavailable snapshot as evidence.
	request.output = std::filesystem::temp_directory_path();
	CHECK_THROWS(WriteGameplayState(request, world, host, scripts, false, observations, &report));
	CHECK(report.artifacts.empty());
	CHECK(report.diagnostics.empty());
}
