#include <doctest/doctest.h>

#include <algorithm>

#include "GameplayPrerequisites.hpp"

using namespace aether;
using namespace aether::app;

TEST_CASE("Gameplay prerequisites do not require Rust tools for CSharp")
{
	const auto report = BuildGameplayPrerequisiteReport(GameplayLanguage::CSharp, {});
	REQUIRE(report.status == scripting::AgentJobStatus::Passed);
	CHECK(report.language == "csharp");
	CHECK(report.profile == "desktop-csharp-v1");
	CHECK(report.diagnostics.empty());
	CHECK(scripting::SerializeAgentReport(report).has_value());
}

TEST_CASE("Gameplay prerequisites report missing Rust tools before any build")
{
	const auto report = BuildGameplayPrerequisiteReport(GameplayLanguage::Rust, {});
	CHECK(report.status == scripting::AgentJobStatus::Failed);
	CHECK(report.language == "rust");
	CHECK(report.profile == "desktop-rust-v1");
	CHECK(report.phase == "preflight");
	CHECK(report.target == "desktop");
	for (const auto* code: {"AE_RUST_CARGO_MISSING", "AE_RUST_RUSTC_MISSING", "AE_RUST_SDK_MISSING", "AE_RUST_BACKEND_UNAVAILABLE"})
	{
		CHECK(std::any_of(report.diagnostics.begin(), report.diagnostics.end(), [code](const auto& diagnostic) { return diagnostic.code == code; }));
	}
	const auto json = scripting::SerializeAgentReport(report);
	REQUIRE(json);
	CHECK((*json)["exitCode"] == 1);
}

TEST_CASE("Gameplay prerequisites do not claim a Rust runtime when tools exist")
{
	const auto report = BuildGameplayPrerequisiteReport(GameplayLanguage::Rust, {.cargo = "cargo", .rustc = "rustc", .rustSdkManifest = "sdk/Cargo.toml"});
	CHECK(report.status == scripting::AgentJobStatus::Failed);
	REQUIRE(report.diagnostics.size() == 1);
	CHECK(report.diagnostics.front().code == "AE_RUST_BACKEND_UNAVAILABLE");
	CHECK(scripting::SerializeAgentReport(report).has_value());
}
