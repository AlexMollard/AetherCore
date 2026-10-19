#include "ProjectPipeline.hpp"
#include "DesktopGameplayRun.hpp"

#include <chrono>
#include <regex>
#include <sstream>
#include <set>
#include <stdexcept>

#include "Defines.hpp"
#include "EngineContentPaths.hpp"
#include "GameplayPrerequisites.hpp"
#include "RuntimeProjectSettings.hpp"
#include "editor/publish/PublishPlan.hpp"
#include "editor/publish/PublishSteps.hpp"
#include "io/FileUtil.hpp"
#include "io/PlatformPaths.hpp"
#include "io/Process.hpp"
#include "project/ProjectCommon.hpp"
#include "project/ProjectStartupScene.hpp"
#include "scripting/DotNetHost.hpp"
#include "scripting/GameplayPipelineRequest.hpp"
#include "scripting/GameplayRun.hpp"
#include "scripting/RustGameplayBuild.hpp"

namespace aether::project_tool
{
	namespace
	{
		using Json = nlohmann::json;
		using Status = scripting::AgentJobStatus;
		std::string Utf8(const std::filesystem::path& path)
		{
			const auto value = path.generic_u8string();
			return {value.begin(), value.end()};
		}
		std::filesystem::path RuntimeWorkingDirectory()
		{
			const auto executableDir = io::PlatformPaths::GetExecutableDir();
			for (auto candidate = executableDir; !candidate.empty(); candidate = candidate.parent_path())
			{
				if (std::filesystem::is_regular_file(candidate / "CMakeCache.txt")) { return candidate; }
				if (candidate == candidate.parent_path()) { break; }
			}
			return executableDir;
		}
		void SetProjectEnvironment(const std::string& project)
		{
#ifdef _WIN32
			_wputenv_s(L"AETHER_PROJECT_DIR", std::filesystem::u8path(project).c_str());
			_wputenv_s(L"AETHER_AGENT_SESSION", L"1");
#else
			setenv("AETHER_PROJECT_DIR", project.c_str(), 1);
			setenv("AETHER_AGENT_SESSION", "1", 1);
#endif
		}
		Json Serialize(const scripting::AgentReport& report)
		{
			const auto serialized = scripting::SerializeAgentReport(report);
			if (!serialized) { throw std::runtime_error(serialized.error().message); }
			return *serialized;
		}
		std::filesystem::path Tool(std::string_view environment, std::string_view name)
		{
			const auto overridden = io::PlatformPaths::ReadEnvironmentVariable(environment);
			if (!overridden.empty())
			{
				const auto path = std::filesystem::u8path(overridden);
				return std::filesystem::is_regular_file(path) ? path : std::filesystem::path{};
			}
			const auto paths = io::PlatformPaths::ReadEnvironmentVariable("PATH");
#ifdef _WIN32
			constexpr char separator = ';';
			const auto filename = std::string(name) + ".exe";
#else
			constexpr char separator = ':';
			const std::string filename(name);
#endif
			std::size_t begin = 0;
			while (begin < paths.size())
			{
				const auto end = paths.find(separator, begin);
				auto entry = paths.substr(begin, end == std::string::npos ? end : end - begin);
				if (entry.size() > 1 && entry.front() == '"' && entry.back() == '"') { entry = entry.substr(1, entry.size() - 2); }
				const auto candidate = std::filesystem::u8path(entry) / filename;
				if (!entry.empty() && std::filesystem::is_regular_file(candidate)) { return candidate; }
				if (end == std::string::npos) { break; }
				begin = end + 1;
			}
			return {};
		}
		void ManagedDiagnostics(const std::filesystem::path& log, scripting::AgentReport& report)
		{
			const auto text = io::file_util::ReadText(log);
			if (!text) { return; }
			std::istringstream lines(*text);
			std::string line;
			const std::regex pattern(R"(^\s*(.+)\(([0-9]+),([0-9]+)\): error (CS[0-9]+): (.+))");
			std::set<std::string> seen;
			while (std::getline(lines, line))
			{
				std::smatch match;
				if (!std::regex_search(line, match, pattern) || !seen.insert(match.str()).second) { continue; }
				report.diagnostics.push_back({.code = match[4].str(), .severity = scripting::AgentDiagnosticSeverity::Error, .message = match[5].str(),
				        .source = scripting::AgentSourceLocation{.file = match[1].str(), .line = static_cast<std::uint32_t>(std::stoul(match[2].str())), .column = static_cast<std::uint32_t>(std::stoul(match[3].str()))}});
			}
		}
	}

	Json ExecuteChild(const Json& request, bool publish)
	{
		const auto project = std::filesystem::u8path(request.at("project").get<std::string>());
		SetProjectEnvironment(Utf8(project));
		if (!publish)
		{
			const app::scripting::GameplayRunRequest runRequest{.project = project,
			        .module = std::filesystem::u8path(request.value("module", "")),
			        .managedDir = std::filesystem::u8path(request.value("managedDir", "")),
			        .output = std::filesystem::u8path(request.at("statePath").get<std::string>()),
			        .scene = request.at("scene").get<std::string>(), .ticks = request.at("ticks").get<std::uint32_t>(),
			        .step = request.at("step").get<float>(), .launchConfig = request.at("launchConfig")};
			return Serialize(runRequest.launchConfig.value("profile", "native-headless-2d-v1") == "native-desktop-2d-v1" ? RunDesktopGameplay(runRequest) : app::scripting::RunGameplay(runRequest));
		}
		const auto descriptor = app::project::ReadProjectDescriptor(project);
		if (!descriptor) { throw std::runtime_error(descriptor.error().message); }
		scripting::AgentReport report{.jobId = request.at("jobId"), .status = Status::Running, .phase = "publish", .target = "desktop", .language = std::string(app::GameplayLanguageName(descriptor->language)), .profile = std::string(app::GameplayProfileName(descriptor->language))};
		scripting::DotNetHost managed;
		if (descriptor->language == app::GameplayLanguage::CSharp && !managed.Initialize(io::PlatformPaths::GetExecutableDir() / "data/scripts/managed")) { throw std::runtime_error("C# publishing requires the bundled managed host"); }
		const auto plan = editor::MakePublishPlan(*descriptor, editor::CurrentPublishEnvironment());
		editor::PublishContext context;
		for (const auto& step: editor::PublishStepList())
		{
			const auto outcome = step.run(plan, context, editor::MakePublishToolchain());
			report.requiredChecks.push_back({std::string(step.name), outcome.ok ? Status::Passed : Status::Failed});
			if (!outcome.ok)
			{
				report.status = Status::Failed;
				report.diagnostics.push_back({"GAMEPLAY_PUBLISH", scripting::AgentDiagnosticSeverity::Error, outcome.message + " " + outcome.remediation});
				return Serialize(report);
			}
		}
		report.status = Status::Passed;
		report.artifacts.push_back({Utf8(plan.outputDir)});
		return Serialize(report);
	}

	Json ExecutePipeline(Json input, const std::filesystem::path& directory, std::stop_token stop, const Progress& progress)
	{
		const auto validated = app::scripting::ValidateGameplayPipelineRequest(input);
		if (!validated) { throw std::runtime_error(validated.error().message); }
		auto request = *validated;
		const auto root = std::filesystem::absolute(std::filesystem::u8path(request.at("project").get<std::string>()));
		request["project"] = Utf8(root);
		SetProjectEnvironment(Utf8(root));
		const auto operation = request.at("operation").get<std::string>();
		const auto started = std::chrono::steady_clock::now();
		const int timeout = request.at("timeoutMs").get<int>();
		const auto remaining = [&] { return std::max(0, timeout - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count())); };
		scripting::AgentReport report{.jobId = request.value("jobId", "gameplay"), .status = Status::Running, .phase = "preflight", .target = "desktop", .language = "csharp", .profile = "desktop-csharp-v1"};
		const auto terminal = [&]
		{
			report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			for (const auto* name: {"build.log", "configure.log", "runtime.log"})
			{
				const auto path = directory / name;
				if (std::filesystem::is_regular_file(path)) { report.artifacts.push_back({Utf8(path), std::filesystem::file_size(path)}); }
			}
			return Serialize(report);
		};
		const auto fail = [&](const std::string& code, const std::string& message)
		{
			report.status = stop.stop_requested() ? Status::Cancelled : Status::Failed;
			report.diagnostics.push_back({stop.stop_requested() ? "GAMEPLAY_CANCELLED" : code, stop.stop_requested() ? scripting::AgentDiagnosticSeverity::Info : scripting::AgentDiagnosticSeverity::Error, message});
			return terminal();
		};
		const auto run = [&](const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::filesystem::path& cwd, const std::filesystem::path& log)
		{
			return io::RunProcessArguments(executable, arguments, cwd, log, remaining(), stop);
		};
		if (stop.stop_requested()) { return fail("GAMEPLAY_CANCELLED", "Cancelled before starting"); }
		if (operation == "create")
		{
			const auto language = app::ParseGameplayLanguage(request.value("language", "csharp"));
			const auto projectTemplate = app::project::ProjectTemplateFromName(request.value("template", "blank_2d"));
			if (!language || !projectTemplate) { return fail("GAMEPLAY_CREATE_ARGUMENT", "Invalid project language or template"); }
			if (app::project::HasProjectDescriptor(root)) { return fail("GAMEPLAY_PROJECT_EXISTS", "Create requires a new project; existing content was preserved"); }
			std::string error;
			if (!app::project::WriteProjectDescriptor(root, request.value("name", root.filename().string()), error, *projectTemplate, *language)) { return fail("GAMEPLAY_CREATE", error); }
		}
		const auto descriptor = app::project::ReadProjectDescriptor(root);
		if (!descriptor) { return fail("GAMEPLAY_PROJECT", descriptor.error().message); }
		report.language = std::string(app::GameplayLanguageName(descriptor->language));
		report.profile = std::string(app::GameplayProfileName(descriptor->language));
		if (operation == "create" || operation == "inspect")
		{
			report.status = Status::Passed;
			report.requiredChecks.push_back({"project-descriptor", Status::Passed});
			report.artifacts.push_back({Utf8(descriptor->projectFile)});
			return terminal();
		}
		request["jobId"] = report.jobId;
		if (operation != "publish")
		{
			report.phase = "build";
			progress(Serialize(report));
			if (descriptor->language == app::GameplayLanguage::Rust)
			{
				const auto built = app::scripting::BuildRustGameplay({.manifest = descriptor->scriptsDir / "Cargo.toml", .artifactsDir = directory / "build/rust", .outputDir = directory / "module", .timeoutMs = remaining(), .stopToken = stop});
				report.toolchains = built.report.toolchains;
				report.diagnostics = built.report.diagnostics;
				report.requiredChecks = built.report.requiredChecks;
				report.artifacts = built.report.artifacts;
				if (!built.Succeeded()) { report.status = built.report.status; return terminal(); }
				request["module"] = Utf8(built.module);
			}
			else
			{
				const auto cmake = Tool("AETHERCORE_CMAKE", "cmake");
				const auto dotnet = Tool("AETHERCORE_DOTNET", "dotnet");
				if (cmake.empty() || dotnet.empty()) { return fail("CSHARP_BUILD_TOOL_MISSING", "C# CLI builds require CMake and the .NET SDK; editor Roslyn compilation remains available"); }
				const auto managed = directory / "managed";
				const auto sdk = io::PlatformPaths::GetExecutableDir() / "data/scripts/managed";
				if (!std::filesystem::is_regular_file(sdk / "AetherCore.Interop.dll")) { return fail("CSHARP_SDK_MISSING", "Bundled C# host is missing"); }
				std::filesystem::create_directories(managed);
				std::filesystem::copy(sdk, managed, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
				const auto versionLog = directory / "dotnet-version.log";
				if (run(dotnet, {"--version"}, root, versionLog) != 0) { return fail("CSHARP_TOOLCHAIN", "Could not query the .NET SDK version"); }
				const auto version = io::file_util::ReadText(versionLog);
				auto identity = version ? *version : std::string("unknown");
				while (!identity.empty() && (identity.back() == '\r' || identity.back() == '\n')) { identity.pop_back(); }
				report.toolchains.push_back({"dotnet", identity, "desktop"});
				auto driver = io::PlatformPaths::GetExecutableDir() / "data/sdk/managed-build";
				if (!std::filesystem::exists(driver)) { driver = AETHER_MANAGED_BUILD_PROJECT; }
				const auto cmakeBuild = directory / "build/managed";
				const std::vector<std::string> configure{"-S", Utf8(driver), "-B", Utf8(cmakeBuild), "-DAETHER_GAMEPLAY_PROJECT=" + Utf8(descriptor->scriptsDir / "AetherGame.csproj"), "-DAETHER_DOTNET_EXECUTABLE=" + Utf8(dotnet), "-DAETHER_MANAGED_ARTIFACTS=" + Utf8(directory / "build/dotnet"), "-DAETHER_MANAGED_OUTPUT=" + Utf8(managed)};
				if (run(cmake, configure, root, directory / "configure.log") != 0) { return fail("CSHARP_CONFIGURE", "C# CMake configuration failed; inspect configure.log"); }
				const int built = run(cmake, {"--build", Utf8(cmakeBuild), "--config", "Release", "--target", "AetherManagedGameplay"}, root, directory / "build.log");
				ManagedDiagnostics(directory / "build.log", report);
				if (built != 0 || !std::filesystem::is_regular_file(managed / "AetherGame.dll")) { return fail(built == io::kProcessTimedOut ? "GAMEPLAY_TIMEOUT" : "CSHARP_BUILD", "C# compilation failed; inspect build.log"); }
				request["managedDir"] = Utf8(managed);
				report.requiredChecks.push_back({"managed-compilation", Status::Passed});
				report.artifacts.push_back({Utf8(managed / "AetherGame.dll")});
			}
		}
		if (operation == "build") { report.status = Status::Passed; return terminal(); }
		if (stop.stop_requested()) { return fail("GAMEPLAY_CANCELLED", "Cancelled before launch"); }
		if (request.value("scene", "").empty()) { request["scene"] = app::ReadProjectStartupScene(descriptor->projectFile); }
		if (operation == "test" && request.at("launchConfig").value("assertions", Json::array()).empty()) { return fail("GAMEPLAY_ASSERTIONS_REQUIRED", "Test requires explicit state assertions in launchConfig"); }
		request["statePath"] = Utf8(directory / "state.json");
		const auto childRequest = directory / "child-request.json";
		const auto childReport = directory / "child-report.json";
		if (!io::file_util::WriteText(childRequest, request.dump(2))) { return fail("GAMEPLAY_IO", "Cannot write child launch request"); }
		report.phase = operation == "publish" ? "publish" : "run";
		progress(Serialize(report));
		auto executable = io::PlatformPaths::GetExecutableDir() / io::PlatformPaths::GetExecutableName();
#ifdef _WIN32
		executable += ".exe";
#endif
		const int exited = run(executable, {operation == "publish" ? "--publish-child" : "--run-child", "--request", Utf8(childRequest), "--report", Utf8(childReport)}, RuntimeWorkingDirectory(), directory / "runtime.log");
		if (exited == io::kProcessCancelled) { return fail("GAMEPLAY_CANCELLED", "Cancelled and closed the owned runtime process"); }
		if (exited == io::kProcessTimedOut) { return fail("GAMEPLAY_TIMEOUT", "Gameplay child exceeded the shared timeout and was closed"); }
		const auto text = io::file_util::ReadText(childReport);
		if (!text) { return fail("GAMEPLAY_CHILD_EXIT", "Gameplay child exited without a terminal report (exit " + std::to_string(exited) + "); inspect runtime.log"); }
		auto child = Json::parse(*text);
		if (exited != 0 && child.value("status", "") == "passed") { return fail("GAMEPLAY_CHILD_EXIT", "Gameplay child failed after producing a passed report"); }
		child["jobId"] = report.jobId;
		child["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		const auto buildReport = Serialize(report);
		for (const auto& item: buildReport["toolchains"]) { child["toolchains"].push_back(item); }
		for (const auto& item: buildReport["artifacts"]) { child["artifacts"].push_back(item); }
		for (const auto& item: buildReport["requiredChecks"]) { child["requiredChecks"].push_back(item); }
		child["artifacts"].push_back({{"path", Utf8(directory / "runtime.log")}, {"bytes", std::filesystem::file_size(directory / "runtime.log")}});
		return child;
	}
}
