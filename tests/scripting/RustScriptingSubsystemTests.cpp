#include <doctest/doctest.h>

#include <chrono>
#include <future>
#include <fstream>

#include "scene/World.hpp"
#include "scripting/RustScriptingSubsystem.hpp"
#include "scripting/SceneContext.hpp"
#include "io/PlatformPaths.hpp"

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

TEST_CASE("Isolated Rust ignores inherited source roots and compiles only an explicit project")
{
	using aether::io::PlatformPaths;
	const auto previousMode = PlatformPaths::ReadEnvironmentVariable("AETHER_ISOLATED_PRODUCT");
	const auto previousEnvironment = PlatformPaths::ReadEnvironmentPath("AETHER_PROJECT_DIR");
	const auto previousExplicit = PlatformPaths::GetExplicitProjectDirectory();
	struct Restore
	{
		std::string mode;
		std::filesystem::path environment;
		std::filesystem::path explicitProject;
		~Restore()
		{
#ifdef _WIN32
			_putenv_s("AETHER_ISOLATED_PRODUCT", mode.c_str());
			_wputenv_s(L"AETHER_PROJECT_DIR", environment.c_str());
#else
			if (mode.empty()) { unsetenv("AETHER_ISOLATED_PRODUCT"); } else { setenv("AETHER_ISOLATED_PRODUCT", mode.c_str(), 1); }
			if (environment.empty()) { unsetenv("AETHER_PROJECT_DIR"); } else { setenv("AETHER_PROJECT_DIR", environment.c_str(), 1); }
#endif
			PlatformPaths::SetExplicitProjectDirectory(explicitProject);
		}
	} restore{previousMode, previousEnvironment, previousExplicit};
	RustSubsystemProject inherited;
#ifdef _WIN32
	REQUIRE(_putenv_s("AETHER_ISOLATED_PRODUCT", "1") == 0);
	REQUIRE(_wputenv_s(L"AETHER_PROJECT_DIR", inherited.root.c_str()) == 0);
#else
	REQUIRE(setenv("AETHER_ISOLATED_PRODUCT", "1", 1) == 0);
	REQUIRE(setenv("AETHER_PROJECT_DIR", inherited.root.c_str(), 1) == 0);
#endif
	PlatformPaths::SetExplicitProjectDirectory({});
	aether::World world;
	SceneContext context{.world = &world};
	int builds = 0;
	const auto build = [&](const RustBuildRequest& request) { ++builds; return BuiltMissingModule(request); };
	{
		RustScriptingSubsystem subsystem(inherited.root, build);
		(void) subsystem.Initialize(context);
		CHECK(builds == 0);
		CHECK_FALSE(subsystem.BeginRebuild());
	}
	PlatformPaths::SetExplicitProjectDirectory(inherited.root);
	{
		RustScriptingSubsystem subsystem({}, build);
		CHECK_FALSE(subsystem.Initialize(context));
		CHECK(builds == 1);
	}
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
