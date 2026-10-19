#include "GameplayPrerequisites.hpp"

#include <string>
#include <string_view>

#include "EngineContentPaths.hpp"
#include "io/PlatformPaths.hpp"
#include "project/ProjectCommon.hpp"

namespace aether::app
{
	namespace
	{
		bool IsExecutable(const std::filesystem::path& path)
		{
			std::error_code ec;
			if (!std::filesystem::is_regular_file(path, ec)) { return false; }
#ifndef _WIN32
			const auto permissions = std::filesystem::status(path, ec).permissions();
			const auto executable = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
			return !ec && (permissions & executable) != std::filesystem::perms::none;
#else
			return true;
#endif
		}

		std::filesystem::path FindExecutable(std::string_view overrideName, std::string_view executable)
		{
			if (const std::string overridePath = io::PlatformPaths::ReadEnvironmentVariable(overrideName); !overridePath.empty())
			{
				return IsExecutable(overridePath) ? std::filesystem::path(overridePath) : std::filesystem::path{};
			}
			const std::string paths = io::PlatformPaths::ReadEnvironmentVariable("PATH");
#ifdef _WIN32
			constexpr char kSeparator = ';';
			const std::string fileName = std::string(executable) + ".exe";
#else
			constexpr char kSeparator = ':';
			const std::string fileName(executable);
#endif
			std::size_t begin = 0;
			while (begin < paths.size())
			{
				const auto end = paths.find(kSeparator, begin);
				std::string directory = paths.substr(begin, end == std::string::npos ? end : end - begin);
				if (directory.size() >= 2 && directory.front() == '"' && directory.back() == '"') { directory = directory.substr(1, directory.size() - 2); }
				if (!directory.empty())
				{
					const auto candidate = std::filesystem::path(directory) / fileName;
					if (IsExecutable(candidate)) { return candidate; }
				}
				if (end == std::string::npos) { break; }
				begin = end + 1;
			}
			return {};
		}
	} // namespace

	GameplayPrerequisites InspectGameplayPrerequisites(GameplayLanguage language)
	{
		if (language != GameplayLanguage::Rust) { return {}; }
		return {FindExecutable("AETHERCORE_CARGO", "cargo"), FindExecutable("AETHERCORE_RUSTC", "rustc"), EngineRustGameplaySdkManifest(), FindExecutable("AETHERCORE_CMAKE", "cmake")};
	}

	::aether::scripting::AgentReport BuildGameplayPrerequisiteReport(GameplayLanguage language, const GameplayPrerequisites& prerequisites)
	{
		using namespace ::aether::scripting;
		AgentReport report{.jobId = "gameplay-prerequisites", .status = AgentJobStatus::Passed, .phase = "preflight", .target = "desktop", .language = std::string(GameplayLanguageName(language)), .profile = std::string(GameplayProfileName(language))};
		const auto check = [&](const char* name, bool passed, const char* code, const char* message)
		{
			report.requiredChecks.push_back({name, passed ? AgentJobStatus::Passed : AgentJobStatus::Failed});
			if (!passed)
			{
				report.status = AgentJobStatus::Failed;
				report.diagnostics.push_back({.code = code, .severity = AgentDiagnosticSeverity::Error, .message = message});
			}
		};
		if (language == GameplayLanguage::Rust)
		{
			check("cargo-executable", !prerequisites.cargo.empty(), "AE_RUST_CARGO_MISSING", "Cargo was not found. Install the Rust toolchain or set AETHERCORE_CARGO to its executable.");
			check("rustc-executable", !prerequisites.rustc.empty(), "AE_RUST_RUSTC_MISSING", "rustc was not found. Install the Rust toolchain or set AETHERCORE_RUSTC to its executable.");
			check("rust-sdk-manifest", !prerequisites.rustSdkManifest.empty(), "AE_RUST_SDK_MISSING", "The AetherCore Rust ABI SDK was not found. Repair the engine install or set AETHERCORE_RUST_GAMEPLAY_SDK to its Cargo.toml.");
			check("cmake-executable", !prerequisites.cmake.empty(), "AE_RUST_CMAKE_MISSING", "CMake was not found. Install CMake or set AETHERCORE_CMAKE to its executable.");
		}
		else
		{
			report.requiredChecks.push_back({"csharp-gameplay-implementation", AgentJobStatus::Passed});
		}
		return report;
	}

	nlohmann::json DescribeGameplayCapabilities(GameplayLanguage language)
	{
		const auto prerequisites = InspectGameplayPrerequisites(language);
		const bool implemented = true;
		const auto report = ::aether::scripting::SerializeAgentReport(BuildGameplayPrerequisiteReport(language, prerequisites));
		if (!report) { return {{"error", report.error().message}}; }
		return {{"language", GameplayLanguageName(language)}, {"profile", GameplayProfileName(language)}, {"target", "desktop"}, {"runtimeImplemented", implemented},
		        {"operations", {{"create", language == GameplayLanguage::CSharp || !prerequisites.rustSdkManifest.empty()}, {"inspect", true}, {"build", implemented}, {"run", implemented}, {"publish", implemented}}},
		        {"prerequisiteReport", *report}};
	}

	nlohmann::json DescribeProjectGameplayCapabilities(const std::filesystem::path& root, std::string_view requestedLanguage)
	{
		GameplayLanguage language = GameplayLanguage::CSharp;
		if (!requestedLanguage.empty())
		{
			const auto parsed = ParseGameplayLanguage(requestedLanguage);
			if (!parsed) { return {{"error", parsed.error().message}}; }
			language = *parsed;
		}
		if (!root.empty())
		{
			const auto descriptor = project::ReadProjectDescriptor(project::ResolveProjectRoot(root));
			if (!descriptor) { return {{"error", descriptor.error().message}}; }
			if (!requestedLanguage.empty() && language != descriptor->language) { return {{"error", "The requested language differs from the project's scripting.language."}}; }
			language = descriptor->language;
		}
		return DescribeGameplayCapabilities(language);
	}
} // namespace aether::app
