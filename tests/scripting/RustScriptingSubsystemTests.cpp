#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <fstream>

#include "scene/World.hpp"
#include "scripting/RustScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"

using namespace aether::app::scripting;

namespace
{
	struct RustSubsystemProject
	{
		std::filesystem::path root = std::filesystem::temp_directory_path() / ("aether-rust-subsystem-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

		RustSubsystemProject()
		{
			std::filesystem::create_directories(root / "scripts");
			std::ofstream(root / "ProjectSettings.toml") << "[scripting]\nlanguage='rust'\n";
			std::ofstream(root / "scripts" / "Cargo.toml") << "[package]\nname='test'\nversion='0.1.0'\n";
		}

		~RustSubsystemProject()
		{
			std::error_code error;
			std::filesystem::remove_all(root, error);
		}
	};

	struct BuildSignals
	{
		std::promise<void> release;
		std::shared_future<void> released = release.get_future().share();
		std::promise<void> entered;
		std::promise<void> completed;
		int calls = 0;
	};

	RustBuildResult BuiltMissingModule(const RustBuildRequest& request)
	{
		RustBuildResult result;
		result.report = {.jobId = "fake-rust-build", .status = aether::scripting::AgentJobStatus::Passed, .phase = "build", .target = "desktop", .language = "rust", .profile = "desktop-rust-v1"};
		result.module = request.outputDir / "missing-gameplay-library";
		return result;
	}
} // namespace

TEST_CASE("Rust module staging failures replace the passed build report")
{
	RustSubsystemProject project;
	aether::World world;
	SceneContext context{.world = &world};
	RustScriptingSubsystem subsystem(project.root, BuiltMissingModule);
	CHECK_FALSE(subsystem.Initialize(context));
	CHECK(subsystem.Report().status == aether::scripting::AgentJobStatus::Failed);
	CHECK(subsystem.Report().phase == "load");
	REQUIRE_FALSE(subsystem.Report().diagnostics.empty());
	CHECK(subsystem.Report().diagnostics.back().code == "RUST_MODULE_STAGE");
	CHECK_FALSE(subsystem.Gameplay().IsAvailable());
	CHECK(aether::scripting::SerializeAgentReport(subsystem.Report()));
}

TEST_CASE("Cancelled Rust compilation stays terminal after the worker completes")
{
	RustSubsystemProject project;
	aether::World world;
	SceneContext context{.world = &world};
	const auto signals = std::make_shared<BuildSignals>();
	auto started = signals->entered.get_future();
	auto finished = signals->completed.get_future();
	RustScriptingSubsystem subsystem(project.root,
	        [signals](const RustBuildRequest& request)
	        {
		        if (++signals->calls > 1)
		        {
			        signals->entered.set_value();
			        signals->released.wait();
			        signals->completed.set_value();
		        }
		        return BuiltMissingModule(request);
	        });
	CHECK_FALSE(subsystem.Initialize(context));
	REQUIRE(subsystem.BeginRebuild());
	const auto workerStarted = started.wait_for(std::chrono::seconds(5));
	subsystem.ClearRebuild();
	CHECK(subsystem.Report().status == aether::scripting::AgentJobStatus::Cancelled);
	CHECK_FALSE(subsystem.BeginRebuild());
	signals->release.set_value();
	CHECK(workerStarted == std::future_status::ready);
	REQUIRE(finished.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	CHECK(subsystem.PollRebuild() == RustScriptingSubsystem::BuildStatus::Failed);
	CHECK(subsystem.Report().status == aether::scripting::AgentJobStatus::Cancelled);
	CHECK_FALSE(subsystem.LoadBuiltModule());
	CHECK_FALSE(subsystem.Gameplay().IsAvailable());
	CHECK(aether::scripting::SerializeAgentReport(subsystem.Report()));
}
