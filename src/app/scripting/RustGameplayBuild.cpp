#include "scripting/RustGameplayBuild.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string_view>
#include <vector>

#include "EngineContentPaths.hpp"
#include "GameplayPrerequisites.hpp"
#include "RuntimeProjectSettings.hpp"
#include "io/FileUtil.hpp"
#include "io/Process.hpp"

namespace aether::app::scripting
{
	namespace
	{
		std::string Utf8(const std::filesystem::path& path)
		{
			const auto text = path.generic_u8string();
			return {text.begin(), text.end()};
		}
		std::string IdentityLine(std::string_view log, std::string_view prefix)
		{
			std::istringstream lines{std::string(log)};
			std::string line;
			while (std::getline(lines, line))
			{
				if (!line.empty() && line.back() == '\r') { line.pop_back(); }
				if (line.starts_with(prefix)) { return line.substr(prefix.size()); }
			}
			return {};
		}
		void CompilerDiagnostics(std::string_view log, aether::scripting::AgentReport& report)
		{
			std::istringstream lines{std::string(log)};
			std::string line;
			while (std::getline(lines, line))
			{
				const auto start = line.find('{');
				if (start == std::string::npos) { continue; }
				const auto item = nlohmann::json::parse(line.substr(start), nullptr, false);
				if (item.is_discarded() || item.value("reason", "") != "compiler-message") { continue; }
				const auto& message = item.at("message");
				if (message.value("level", "") != "error") { continue; }
				aether::scripting::AgentDiagnostic diagnostic{.code = "RUST_COMPILER_ERROR", .severity = aether::scripting::AgentDiagnosticSeverity::Error, .message = message.value("message", "Rust compilation failed")};
				for (const auto& span: message.value("spans", nlohmann::json::array()))
				{
					if (span.value("is_primary", false))
					{
						diagnostic.source = aether::scripting::AgentSourceLocation{.file = span.value("file_name", "unknown"), .line = span.value("line_start", 1u), .column = span.value("column_start", 1u)};
						break;
					}
				}
				report.diagnostics.push_back(std::move(diagnostic));
			}
		}
	}

	RustBuildResult BuildRustGameplay(const RustBuildRequest& request)
	{
		using namespace aether::scripting;
		RustBuildResult result;
		result.report = {.jobId = "rust-build", .status = AgentJobStatus::Running, .phase = "preflight", .target = "desktop", .language = "rust", .profile = "desktop-rust-v1"};
		const auto started = std::chrono::steady_clock::now();
		const auto remaining = [&] { return std::max(0, request.timeoutMs - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count())); };
		const auto fail = [&](std::string code, std::string message)
		{
			result.report.status = request.stopToken.stop_requested() ? AgentJobStatus::Cancelled : AgentJobStatus::Failed;
			result.report.diagnostics.push_back({.code = request.stopToken.stop_requested() ? "RUST_BUILD_CANCELLED" : std::move(code), .severity = request.stopToken.stop_requested() ? AgentDiagnosticSeverity::Info : AgentDiagnosticSeverity::Error, .message = std::move(message)});
			result.report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
			return result;
		};
		if (request.stopToken.stop_requested()) { return fail("RUST_BUILD_CANCELLED", "Cancelled before Rust build"); }
		std::error_code ec;
		if (!std::filesystem::is_regular_file(request.manifest, ec)) { return fail("RUST_MANIFEST_MISSING", "Gameplay Cargo.toml is missing: " + Utf8(request.manifest)); }
		if (request.artifactsDir.empty() || request.outputDir.empty() || request.timeoutMs <= 0) { return fail("RUST_BUILD_ARGUMENT", "Rust builds require explicit output/intermediate directories and a positive timeout."); }
		const auto prerequisites = InspectGameplayPrerequisites(GameplayLanguage::Rust);
		result.report = BuildGameplayPrerequisiteReport(GameplayLanguage::Rust, prerequisites);
		result.report.jobId = "rust-build";
		if (result.report.status == AgentJobStatus::Failed) { return result; }
		const auto driver = EngineRustBuildProjectDir();
		if (driver.empty()) { return fail("RUST_BUILD_DRIVER_MISSING", "The engine Rust CMake build driver is missing; repair the engine SDK installation."); }
		const auto artifacts = std::filesystem::absolute(request.artifactsDir);
		const auto output = std::filesystem::absolute(request.outputDir);
		const auto cargoTarget = request.cargoTargetDir.empty() ? artifacts / "cargo" : std::filesystem::absolute(request.cargoTargetDir);
		const auto manifest = std::filesystem::absolute(request.manifest);
		const auto configureLog = artifacts / "logs" / "configure.log";
		const auto buildLog = artifacts / "logs" / "build.log";
		const auto build = artifacts / "cmake";
		result.report.status = AgentJobStatus::Running;
		result.report.phase = "toolchain";
		const auto rustcLog = artifacts / "logs" / "rustc-version.log";
		const auto cargoLog = artifacts / "logs" / "cargo-version.log";
		const std::vector<std::string> rustcArguments{"-vV"};
		const int rustcBudget = remaining();
		if (rustcBudget <= 0) { return fail("RUST_BUILD_TIMEOUT", "Rust toolchain discovery exceeded the build timeout."); }
		const int rustcQueried = io::RunProcessArguments(prerequisites.rustc, rustcArguments, manifest.parent_path(), rustcLog, rustcBudget, request.stopToken);
		if (rustcQueried != 0) { return fail(rustcQueried == io::kProcessTimedOut ? "RUST_BUILD_TIMEOUT" : "RUST_TOOLCHAIN_QUERY_FAILED", "rustc identity query failed; inspect " + Utf8(rustcLog)); }
		const auto rustcIdentity = io::file_util::ReadText(rustcLog);
		const auto rustcVersion = rustcIdentity ? IdentityLine(*rustcIdentity, "rustc ") : std::string{};
		const auto rustHost = rustcIdentity ? IdentityLine(*rustcIdentity, "host: ") : std::string{};
		if (rustcVersion.empty() || rustHost.empty()) { return fail("RUST_TOOLCHAIN_IDENTITY_INVALID", "rustc -vV did not report its version and host triple; inspect " + Utf8(rustcLog)); }
		result.report.toolchains.push_back({.name = "rustc", .version = rustcVersion, .target = rustHost});
		const std::vector<std::string> cargoArguments{"--version"};
		const int cargoBudget = remaining();
		if (cargoBudget <= 0) { return fail("RUST_BUILD_TIMEOUT", "Rust toolchain discovery exceeded the build timeout."); }
		const int cargoQueried = io::RunProcessArguments(prerequisites.cargo, cargoArguments, manifest.parent_path(), cargoLog, cargoBudget, request.stopToken);
		if (cargoQueried != 0) { return fail(cargoQueried == io::kProcessTimedOut ? "RUST_BUILD_TIMEOUT" : "RUST_TOOLCHAIN_QUERY_FAILED", "Cargo identity query failed; inspect " + Utf8(cargoLog)); }
		const auto cargoIdentity = io::file_util::ReadText(cargoLog);
		const auto cargoVersion = cargoIdentity ? IdentityLine(*cargoIdentity, "cargo ") : std::string{};
		if (cargoVersion.empty()) { return fail("RUST_TOOLCHAIN_IDENTITY_INVALID", "cargo --version did not report its version; inspect " + Utf8(cargoLog)); }
		result.report.toolchains.push_back({.name = "cargo", .version = cargoVersion, .target = rustHost});
		result.report.phase = "configure";
		const std::vector<std::string> configure{"-S", Utf8(driver), "-B", Utf8(build), "-DAETHER_GAMEPLAY_MANIFEST=" + Utf8(manifest),
		        "-DAETHER_CARGO_EXECUTABLE=" + Utf8(prerequisites.cargo), "-DAETHER_RUSTC_EXECUTABLE=" + Utf8(prerequisites.rustc),
		        "-DAETHER_RUST_TARGET_DIR=" + Utf8(cargoTarget), "-DAETHER_RUST_OUTPUT_DIR=" + Utf8(output), std::string("-DAETHER_RUST_PROFILE=") + (request.release ? "release" : "debug")};
		const int configured = io::RunProcessArguments(prerequisites.cmake, configure, manifest.parent_path(), configureLog, remaining(), request.stopToken);
		if (configured != 0) { return fail(configured == io::kProcessTimedOut ? "RUST_BUILD_TIMEOUT" : "RUST_CONFIGURE_FAILED", "Rust CMake configuration failed; inspect " + Utf8(configureLog)); }
		result.report.phase = "build";
		const std::vector<std::string> args{"--build", Utf8(build), "--config", "Release", "--target", "AetherRustGameplay"};
		const int compiled = io::RunProcessArguments(prerequisites.cmake, args, manifest.parent_path(), buildLog, remaining(), request.stopToken);
		if (const auto log = io::file_util::ReadText(buildLog)) { CompilerDiagnostics(*log, result.report); }
		if (compiled != 0) { return fail(compiled == io::kProcessTimedOut ? "RUST_BUILD_TIMEOUT" : "RUST_BUILD_FAILED", "Rust compilation failed; inspect " + Utf8(buildLog)); }
		const auto module = output / ResolveRuntimeGameplayModule({}, GameplayLanguage::Rust).filename();
		if (!std::filesystem::is_regular_file(module, ec)) { return fail("RUST_ARTIFACT_MISSING", "Cargo completed without the expected aether_gameplay shared library."); }
		result.report.artifacts.push_back({Utf8(module), std::filesystem::file_size(module, ec)});
		result.report.requiredChecks.push_back({"cargo-compilation", AgentJobStatus::Passed});
		result.report.status = AgentJobStatus::Passed;
		result.report.phase = "build";
		result.report.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
		result.module = module;
		return result;
	}
}
