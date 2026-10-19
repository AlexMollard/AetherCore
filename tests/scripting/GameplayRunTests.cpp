#include <doctest/doctest.h>
#include <limits>
#include <chrono>
#include <cstdlib>
#include "io/FileUtil.hpp"
#include "platform/Input.hpp"
#include "scene/SceneSerializer.hpp"
#include "scripting/GameplayRun.hpp"

using namespace aether::app::scripting;
using aether::scripting::AgentJobStatus;

TEST_CASE("Gameplay run rejects unbounded and nonfinite simulation requests before loading")
{
	GameplayRunRequest request;
	request.ticks = 0;
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.ticks = 60;
	request.step = std::numeric_limits<float>::quiet_NaN();
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.step = 0;
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
}

TEST_CASE("Gameplay run rejects malformed launch inputs and assertions")
{
	GameplayRunRequest request;
	request.launchConfig = {{"readSaves", "yes"}};
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.launchConfig = {{"input", {{{"tick", 900}, {"key", "space"}, {"down", true}}}}};
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.launchConfig = {{"assertions", {{{"entity", "Player"}, {"property", "Score"}}}}};
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
}

TEST_CASE("Headless gameplay input preserves press hold and release edges across ticks")
{
	aether::Input input;
	const auto key = static_cast<aether::Key>(aether::Input::KeyCodeFromName("space"));
	input.SetSyntheticKey(static_cast<int>(key), true, false);
	input.Update();
	CHECK(input.IsKeyDown(key));
	CHECK(input.IsKeyPressed(key));
	input.Update();
	CHECK(input.IsKeyDown(key));
	CHECK_FALSE(input.IsKeyPressed(key));
	input.SetSyntheticKey(static_cast<int>(key), false, false);
	input.Update();
	CHECK_FALSE(input.IsKeyDown(key));
	CHECK(input.IsKeyReleased(key));
}

TEST_CASE("Headless gameplay rejects interactive UI before loading a module without a layout viewport")
{
	const auto root = std::filesystem::absolute(std::filesystem::path(".temp/task6") / ("unsupported-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
	REQUIRE(aether::io::file_util::CreateDirectories(root / "scenes"));
	REQUIRE(aether::io::file_util::CreateDirectories(root / "scripts"));
	REQUIRE(aether::io::file_util::WriteText(root / "ProjectSettings.toml", "[project]\nname = 'Unsupported UI fixture'\n[scripting]\nlanguage = 'rust'\n"));
	REQUIRE(aether::io::file_util::WriteText(root / "scripts/Cargo.toml", "[package]\nname = 'fixture'\nversion = '0.1.0'\n"));
	aether::app::scene::SceneDescription scene;
	scene.kind = aether::SceneKind::Scene2D;
	scene.features = aether::DefaultSceneFeatures(scene.kind);
	aether::app::scene::EntityRecord selectable;
	selectable.name = "Needs UI layout";
	selectable.reflected.push_back({"UI Selectable", {{"interactable", {.type = aether::reflect::FieldType::Bool, .boolean = true}}}});
	scene.entities.push_back(selectable);
	const auto serialized = aether::app::scene::WriteToml(scene);
	const auto parsed = aether::app::scene::ParseToml(serialized);
	REQUIRE(parsed);
	REQUIRE(parsed->entities.size() == 1);
	REQUIRE(parsed->entities[0].reflected.size() == 1);
	REQUIRE(aether::io::file_util::WriteText(root / "scenes/Start.scene.toml", serialized));
	GameplayRunRequest request;
	request.project = root;
	request.scene = "Start";
	request.module = root / "deliberately-missing.dll";
	request.output = root / "state.json";
	const auto result = RunGameplay(request);
	REQUIRE(result.status == AgentJobStatus::Failed);
	REQUIRE_FALSE(result.diagnostics.empty());
	CHECK(result.diagnostics.back().message.find("Headless run does not support component: UI Selectable") != std::string::npos);
}

TEST_CASE("Headless Rust runs restore authored properties and entity references on every restart")
{
	const char* module = std::getenv("AETHER_RUST_TEST_MODULE");
	if (module == nullptr || *module == '\0')
	{
		MESSAGE("Set AETHER_RUST_TEST_MODULE to execute the bounded native gameplay fixture");
		return;
	}
	const auto root = std::filesystem::absolute(std::filesystem::path(".temp/task6") / ("runner-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
	REQUIRE(aether::io::file_util::CreateDirectories(root / "scenes"));
	REQUIRE(aether::io::file_util::CreateDirectories(root / "scripts"));
	REQUIRE(aether::io::file_util::WriteText(root / "ProjectSettings.toml", "[project]\nname = 'Runner fixture'\n[scripting]\nlanguage = 'rust'\n"));
	REQUIRE(aether::io::file_util::WriteText(root / "scripts/Cargo.toml", "[package]\nname = 'fixture'\nversion = '0.1.0'\n"));
	aether::app::scene::SceneDescription scene;
	scene.kind = aether::SceneKind::Scene2D;
	scene.features = aether::DefaultSceneFeatures(scene.kind);
	aether::app::scene::EntityRecord player;
	player.name = "Player";
	player.hasTransform = true;
	aether::app::scene::ScriptRecord script;
	script.type = "Probe";
	script.properties["ticks"] = {.type = aether::ScriptPropertyValue::Type::Int, .i64 = 7};
	script.properties["target"] = {.type = aether::ScriptPropertyValue::Type::Entity, .i64 = 1};
	script.properties["label"] = {.type = aether::ScriptPropertyValue::Type::String, .str = "authored field"};
	player.scripts.push_back(script);
	scene.entities.push_back(player);
	aether::app::scene::EntityRecord target;
	target.name = "AuthoredTarget";
	scene.entities.push_back(target);
	REQUIRE(aether::io::file_util::WriteText(root / "scenes/Start.scene.toml", aether::app::scene::WriteToml(scene)));
	GameplayRunRequest request;
	request.project = root;
	request.module = module;
	request.output = root / "state.json";
	request.scene = "Start";
	request.ticks = 3;
	request.launchConfig = {{"assertions", {
		{{"entity", "Player"}, {"type", "Probe"}, {"property", "ticks"}, {"equals", 10}},
		{{"entity", "Player"}, {"type", "Probe"}, {"property", "target"}, {"equals", "AuthoredTarget"}},
		{{"entity", "Player"}, {"type", "Probe"}, {"property", "label"}, {"equals", "authored field"}}
	}}};
	for (int run = 0; run < 2; ++run)
	{
		const auto result = RunGameplay(request);
		for (const auto& diagnostic: result.diagnostics)
		{
			INFO(diagnostic.message);
		}
		REQUIRE(result.status == AgentJobStatus::Passed);
		const auto stateText = aether::io::file_util::ReadText(request.output);
		REQUIRE(stateText);
		const auto state = nlohmann::json::parse(*stateText);
		CHECK(state["initialFrame"] == 0);
		CHECK(state["readSaves"] == false);
	}
	request.launchConfig["assertions"][0]["equals"] = 999;
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.launchConfig["assertions"][0]["type"] = "MissingType";
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.launchConfig["assertions"][0]["type"] = "Probe";
	request.launchConfig["assertions"][0]["property"] = "MissingProperty";
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	scene.entities[0].scripts[0].properties["ticks"].i64 = 9007199254740992LL;
	REQUIRE(aether::io::file_util::WriteText(root / "scenes/Start.scene.toml", aether::app::scene::WriteToml(scene)));
	request.ticks = 1;
	request.launchConfig = {{"assertions", {{{"entity", "Player"}, {"type", "Probe"}, {"property", "ticks"}, {"equals", 9007199254740992LL}}}}};
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
	request.launchConfig["assertions"][0]["equals"] = 9007199254740993LL;
	CHECK(RunGameplay(request).status == AgentJobStatus::Passed);
	scene.entities[0].scripts[0].type = "MissingType";
	REQUIRE(aether::io::file_util::WriteText(root / "scenes/Start.scene.toml", aether::app::scene::WriteToml(scene)));
	CHECK(RunGameplay(request).status == AgentJobStatus::Failed);
}
