#include <doctest/doctest.h>

#include <array>
#include <filesystem>
#include <fstream>

#include "scripting/AgentDiagnostics.hpp"
#include "editor/publish/PublishReport.hpp"
#include "scene/World.hpp"
#include "scripting/GameplayAccess.hpp"

namespace
{
	aether::scripting::AgentReport MakeAgentReport()
	{
		aether::scripting::AgentReport report;
		report.jobId = "publish-42";
		report.status = aether::scripting::AgentJobStatus::Passed;
		report.phase = "publish";
		report.target = "windows-x64";
		report.language = "csharp";
		report.profile = "desktop";
		report.durationMs = 321;
		return report;
	}
} // namespace

TEST_CASE("Agent reports serialize the versioned contract with explicit terminal exit codes")
{
	using namespace aether::scripting;
	const std::array statuses{AgentJobStatus::Queued, AgentJobStatus::Running, AgentJobStatus::Passed, AgentJobStatus::Failed, AgentJobStatus::Cancelled, AgentJobStatus::Skipped};
	const std::array<const char*, 6> names{"queued", "running", "passed", "failed", "cancelled", "skipped"};
	const std::array<int, 6> exitCodes{-1, -1, 0, 1, 130, 3};
	for (std::size_t i = 0; i < statuses.size(); ++i)
	{
		auto report = MakeAgentReport();
		report.status = statuses[i];
		if (report.status == AgentJobStatus::Failed)
		{
			report.diagnostics.push_back({"BUILD_FAILED", AgentDiagnosticSeverity::Error, "Compiler failed"});
		}
		const auto json = SerializeAgentReport(report);
		REQUIRE(json.has_value());
		CHECK((*json)["schemaVersion"] == 1);
		CHECK((*json)["jobId"] == "publish-42");
		CHECK((*json)["status"] == names[i]);
		CHECK((*json)["phase"] == "publish");
		CHECK((*json)["target"] == "windows-x64");
		CHECK((*json)["language"] == "csharp");
		CHECK((*json)["profile"] == "desktop");
		CHECK((*json)["durationMs"] == 321);
		CHECK((*json)["diagnostics"].is_array());
		CHECK((*json)["toolchains"].is_array());
		CHECK((*json)["artifacts"].is_array());
		if (exitCodes[i] < 0)
		{
			CHECK((*json)["exitCode"].is_null());
		}
		else
		{
			CHECK((*json)["exitCode"] == exitCodes[i]);
		}
	}
}

TEST_CASE("Gameplay ABI diagnostics become owning validated agent diagnostics")
{
	using namespace aether::scripting;
	for (const auto severity: {AE_GAMEPLAY_DIAGNOSTIC_INFO, AE_GAMEPLAY_DIAGNOSTIC_WARNING, AE_GAMEPLAY_DIAGNOSTIC_ERROR})
	{
		std::string code = "SCRIPT_FAILED";
		std::string message = "caf\xc3\xa9";
		std::string file = "scripts/player.rs";
		const AetherGameplayDiagnostic borrowed{
		        sizeof(AetherGameplayDiagnostic), severity, {code.data(), static_cast<std::uint32_t>(code.size())}, {message.data(), static_cast<std::uint32_t>(message.size())}, {file.data(), static_cast<std::uint32_t>(file.size())}, 12, 3};
		const auto owned = FromGameplayDiagnostic(borrowed);
		REQUIRE(owned.has_value());
		code.assign(code.size(), 'x');
		message.clear();
		file.clear();
		CHECK(owned->code == "SCRIPT_FAILED");
		CHECK(owned->message == "caf\xc3\xa9");
		CHECK(static_cast<std::uint32_t>(owned->severity) == severity);
		REQUIRE(owned->source.has_value());
		CHECK(owned->source->file == "scripts/player.rs");
		CHECK(owned->source->line == 12);
		CHECK(owned->source->column == 3);
	}
}

TEST_CASE("Gameplay diagnostic adapter rejects malformed records and source positions")
{
	using namespace aether::scripting;
	AetherGameplayDiagnostic diagnostic{sizeof(AetherGameplayDiagnostic), AE_GAMEPLAY_DIAGNOSTIC_ERROR, {"ERR", 3}, {"Failure", 7}, {}, 0, 0};
	SUBCASE("short record")
	{
		diagnostic.structSize = sizeof(diagnostic) - 1;
	}
	SUBCASE("unexpected record layout")
	{
		diagnostic.structSize = sizeof(diagnostic) + 1;
	}
	SUBCASE("unknown severity")
	{
		diagnostic.severity = 3;
	}
	SUBCASE("null code with nonzero length")
	{
		diagnostic.code = {nullptr, 3};
	}
	SUBCASE("null message with nonzero length")
	{
		diagnostic.message = {nullptr, 3};
	}
	SUBCASE("empty code")
	{
		diagnostic.code = {};
	}
	SUBCASE("blank message")
	{
		diagnostic.message = {" ", 1};
	}
	SUBCASE("invalid UTF-8 message")
	{
		diagnostic.message = {"\xed\xa0\x80", 3};
	}
	SUBCASE("invalid UTF-8 file")
	{
		diagnostic.file = {"\xff", 1};
	}
	SUBCASE("line without file")
	{
		diagnostic.line = 1;
	}
	SUBCASE("column without line")
	{
		diagnostic.file = {"a.rs", 4};
		diagnostic.column = 1;
	}
	CHECK_FALSE(FromGameplayDiagnostic(diagnostic).has_value());
}

TEST_CASE("Synchronous gameplay diagnostic sink forwards an owning agent report")
{
	using namespace aether::scripting;
	aether::World world;
	auto report = MakeAgentReport();
	report.status = AgentJobStatus::Failed;
	GameplayAccess access(
	        world,
	        [](void* user, const AetherGameplayDiagnostic& borrowed) -> AetherGameplayStatus
	        {
		        auto owned = FromGameplayDiagnostic(borrowed);
		        if (!owned)
		        {
			        return AE_GAMEPLAY_INVALID_ARGUMENT;
		        }
		        static_cast<AgentReport*>(user)->diagnostics.push_back(std::move(*owned));
		        return AE_GAMEPLAY_OK;
	        },
	        &report);
	const auto api = access.EngineApi();
	{
		const std::string message = "Gameplay failed";
		const AetherGameplayDiagnostic borrowed{sizeof(AetherGameplayDiagnostic), AE_GAMEPLAY_DIAGNOSTIC_ERROR, {"SCRIPT_FAILED", 13}, {message.data(), static_cast<std::uint32_t>(message.size())}, {}, 0, 0};
		CHECK(api.reportDiagnostic(api.context, &borrowed) == AE_GAMEPLAY_OK);
	}
	const auto json = SerializeAgentReport(report);
	REQUIRE(json.has_value());
	CHECK((*json)["status"] == "failed");
	CHECK((*json)["diagnostics"][0]["message"] == "Gameplay failed");
}

TEST_CASE("Checked-in agent schema examples match real serializer output")
{
	using namespace aether::scripting;
	for (const auto status: {AgentJobStatus::Passed, AgentJobStatus::Running, AgentJobStatus::Failed})
	{
		auto report = MakeAgentReport();
		report.status = status;
		std::string name = "passed";
		if (status == AgentJobStatus::Running)
		{
			name = "running";
		}
		else if (status == AgentJobStatus::Failed)
		{
			name = "failed";
			report.diagnostics.push_back({"PUBLISH_FAILED", AgentDiagnosticSeverity::Error, "Asset packer failed", AgentSourceLocation{"assets/player.png"}});
		}
		const auto expected = SerializeAgentReport(report);
		REQUIRE(expected.has_value());
		const auto path = std::filesystem::path(AETHER_TESTS_SOURCE_DIR).parent_path() / "docs/schemas/examples" / ("agent-report-v1-" + name + ".json");
		std::ifstream file(path);
		REQUIRE(file.is_open());
		const auto example = nlohmann::json::parse(file);
		CHECK(example == *expected);
	}
}

TEST_CASE("Agent reports preserve UTF-8 diagnostics source positions and artifact provenance")
{
	using namespace aether::scripting;
	auto report = MakeAgentReport();
	report.diagnostics.push_back({"SCRIPT_WARNING", AgentDiagnosticSeverity::Warning, "caf\xc3\xa9 \xe6\xb8\xb8\xe6\x88\x8f", AgentSourceLocation{"scripts/player.rs", 12, 3}});
	report.toolchains.push_back({"rustc", "1.90.0", "wasm32-unknown-emscripten"});
	report.artifacts.push_back({"Builds/game.wasm", 4096, "sha256:abc123"});
	const auto json = SerializeAgentReport(report);
	REQUIRE(json.has_value());
	const nlohmann::json expectedSource{{"file", "scripts/player.rs"}, {"line", 12}, {"column", 3}};
	CHECK((*json)["diagnostics"][0]["source"] == expectedSource);
	CHECK((*json)["diagnostics"][0]["message"] == report.diagnostics[0].message);
	CHECK((*json)["diagnostics"][0]["severity"] == "warning");
	CHECK((*json)["toolchains"][0]["version"] == "1.90.0");
	CHECK((*json)["artifacts"][0]["bytes"] == 4096);
	CHECK(nlohmann::json::parse(json->dump())["diagnostics"][0]["message"] == report.diagnostics[0].message);
}

TEST_CASE("Agent reports reject invalid or contradictory outcomes before serialization")
{
	using namespace aether::scripting;
	auto report = MakeAgentReport();
	SUBCASE("passed with errors")
	{
		report.diagnostics.push_back({"BROKEN", AgentDiagnosticSeverity::Error, "An error occurred"});
	}
	SUBCASE("failed without actionable diagnostics")
	{
		report.status = AgentJobStatus::Failed;
	}
	SUBCASE("unknown status")
	{
		report.status = static_cast<AgentJobStatus>(99);
	}
	SUBCASE("negative elapsed time")
	{
		report.durationMs = -1;
	}
	SUBCASE("missing job identity")
	{
		report.jobId.clear();
	}
	SUBCASE("blank phase")
	{
		report.phase = " \t";
	}
	SUBCASE("missing target")
	{
		report.target.clear();
	}
	SUBCASE("missing language")
	{
		report.language.clear();
	}
	SUBCASE("missing profile")
	{
		report.profile.clear();
	}
	SUBCASE("missing diagnostic code")
	{
		report.diagnostics.push_back({"", AgentDiagnosticSeverity::Warning, "Warning"});
	}
	SUBCASE("unknown severity")
	{
		report.diagnostics.push_back({"WARN", static_cast<AgentDiagnosticSeverity>(99), "Warning"});
	}
	SUBCASE("missing diagnostic message")
	{
		report.diagnostics.push_back({"WARN", AgentDiagnosticSeverity::Warning, ""});
	}
	SUBCASE("zero source line")
	{
		report.diagnostics.push_back({"WARN", AgentDiagnosticSeverity::Warning, "Warning", AgentSourceLocation{"script.rs", 0, 1}});
	}
	SUBCASE("zero source column")
	{
		report.diagnostics.push_back({"WARN", AgentDiagnosticSeverity::Warning, "Warning", AgentSourceLocation{"script.rs", 1, 0}});
	}
	SUBCASE("column without line")
	{
		report.diagnostics.push_back({"WARN", AgentDiagnosticSeverity::Warning, "Warning", AgentSourceLocation{"script.rs", {}, 1}});
	}
	SUBCASE("source without file")
	{
		report.diagnostics.push_back({"WARN", AgentDiagnosticSeverity::Warning, "Warning", AgentSourceLocation{"", 1, 1}});
	}
	SUBCASE("missing tool version")
	{
		report.toolchains.push_back({"rustc", "", "wasm32"});
	}
	SUBCASE("missing artifact path")
	{
		report.artifacts.push_back({""});
	}
	SUBCASE("empty artifact hash")
	{
		report.artifacts.push_back({"game.wasm", {}, ""});
	}
	SUBCASE("unknown check status")
	{
		report.requiredChecks.push_back({"gpu", static_cast<AgentJobStatus>(99)});
	}
	SUBCASE("missing check name")
	{
		report.requiredChecks.push_back({"", AgentJobStatus::Passed});
	}
	SUBCASE("duplicate check names")
	{
		report.requiredChecks.push_back({"gpu", AgentJobStatus::Passed});
		report.requiredChecks.push_back({"gpu", AgentJobStatus::Passed});
	}
	SUBCASE("invalid UTF-8")
	{
		report.jobId = std::string(1, static_cast<char>(0xff));
	}
	const auto json = SerializeAgentReport(report);
	REQUIRE_FALSE(json.has_value());
	CHECK_FALSE(json.error().message.empty());
}

TEST_CASE("Agent reports never aggregate skipped or unfinished required checks into a pass")
{
	using namespace aether::scripting;
	for (auto status: {AgentJobStatus::Queued, AgentJobStatus::Running, AgentJobStatus::Failed, AgentJobStatus::Cancelled, AgentJobStatus::Skipped})
	{
		auto report = MakeAgentReport();
		report.requiredChecks.push_back({"browser-gpu", status});
		CHECK_FALSE(SerializeAgentReport(report).has_value());
	}
	auto report = MakeAgentReport();
	report.requiredChecks.push_back({"browser-gpu", AgentJobStatus::Passed});
	const auto json = SerializeAgentReport(report);
	REQUIRE(json.has_value());
	CHECK((*json)["requiredChecks"][0]["status"] == "passed");
}

TEST_CASE("Publish agent adapter preserves legacy output and distinguishes pending from failure")
{
	using namespace aether::scripting;
	const aether::editor::PublishReport legacy{true, "Published game", "Full report text"};
	auto context = MakeAgentReport();
	const auto success = aether::editor::BuildPublishAgentReport(legacy, context, true);
	REQUIRE(success.has_value());
	CHECK(success->status == AgentJobStatus::Passed);
	CHECK(legacy.ok);
	CHECK(legacy.summary == "Published game");
	CHECK(legacy.text == "Full report text");
	context.status = AgentJobStatus::Running;
	const aether::editor::PublishReport empty;
	const auto pending = aether::editor::BuildPublishAgentReport(empty, context, false);
	REQUIRE(pending.has_value());
	CHECK(pending->status == AgentJobStatus::Running);
	CHECK(pending->diagnostics.empty());
	const auto failure = aether::editor::BuildPublishAgentReport(empty, context, true);
	REQUIRE(failure.has_value());
	CHECK(failure->status == AgentJobStatus::Failed);
	REQUIRE(failure->diagnostics.size() == 1);
	CHECK(failure->diagnostics[0].code == "PUBLISH_FAILED");
	CHECK_FALSE(failure->diagnostics[0].message.empty());
	const aether::editor::PublishReport failedLegacy{false, "Asset packer failed", "Failure details"};
	const auto explainedFailure = aether::editor::BuildPublishAgentReport(failedLegacy, context, true);
	REQUIRE(explainedFailure.has_value());
	CHECK(explainedFailure->diagnostics[0].message == "Asset packer failed");
	CHECK_FALSE(failedLegacy.ok);
	CHECK(failedLegacy.text == "Failure details");
	context.status = AgentJobStatus::Queued;
	const auto queued = aether::editor::BuildPublishAgentReport(empty, context, false);
	REQUIRE(queued.has_value());
	CHECK(queued->status == AgentJobStatus::Queued);
	context.status = AgentJobStatus::Passed;
	CHECK_FALSE(aether::editor::BuildPublishAgentReport(empty, context, false).has_value());
	context.diagnostics.push_back({"BUILD_FAILED", AgentDiagnosticSeverity::Error, "Compiler failed"});
	CHECK_FALSE(aether::editor::BuildPublishAgentReport(legacy, context, true).has_value());
}
