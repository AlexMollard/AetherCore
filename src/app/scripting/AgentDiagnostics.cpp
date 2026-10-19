#include "scripting/AgentDiagnostics.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>

#include "scripting/GameplayAbiValidation.hpp"

namespace aether::scripting
{
	namespace
	{
		std::string_view StatusName(AgentJobStatus status)
		{
			switch (status)
			{
				case AgentJobStatus::Queued:
					return "queued";
				case AgentJobStatus::Running:
					return "running";
				case AgentJobStatus::Passed:
					return "passed";
				case AgentJobStatus::Failed:
					return "failed";
				case AgentJobStatus::Cancelled:
					return "cancelled";
				case AgentJobStatus::Skipped:
					return "skipped";
			}
			return {};
		}

		std::string_view SeverityName(AgentDiagnosticSeverity severity)
		{
			switch (severity)
			{
				case AgentDiagnosticSeverity::Info:
					return "info";
				case AgentDiagnosticSeverity::Warning:
					return "warning";
				case AgentDiagnosticSeverity::Error:
					return "error";
			}
			return {};
		}

		bool HasText(std::string_view value)
		{
			return value.find_first_not_of(" \t\r\n") != std::string_view::npos;
		}

		Unexpected Invalid(std::string_view message)
		{
			return Unexpected{AetherError::Engine(std::string("Invalid agent report: ") + std::string(message))};
		}

		Expected<void> ValidateDiagnostic(const AgentDiagnostic& diagnostic)
		{
			if (SeverityName(diagnostic.severity).empty() || !HasText(diagnostic.code) || !HasText(diagnostic.message))
			{
				return Invalid("diagnostics require a valid severity and nonempty code/message");
			}
			if (diagnostic.source)
			{
				const auto& source = *diagnostic.source;
				if (!HasText(source.file) || (source.line && *source.line == 0) || (source.column && (!source.line || *source.column == 0)))
				{
					return Invalid("source requires a file and one-based positions; column requires line");
				}
			}
			return {};
		}
	} // namespace

	Expected<AgentDiagnostic> FromGameplayDiagnostic(const AetherGameplayDiagnostic& diagnostic)
	{
		if (diagnostic.structSize != sizeof(AetherGameplayDiagnostic))
		{
			return Invalid("gameplay diagnostic has an unsupported v1 struct size");
		}
		if (!IsGameplayUtf8(diagnostic.code) || !IsGameplayUtf8(diagnostic.message) || !IsGameplayUtf8(diagnostic.file))
		{
			return Invalid("gameplay diagnostic contains an invalid UTF-8 view");
		}
		AgentDiagnostic result;
		switch (diagnostic.severity)
		{
			case AE_GAMEPLAY_DIAGNOSTIC_INFO:
				result.severity = AgentDiagnosticSeverity::Info;
				break;
			case AE_GAMEPLAY_DIAGNOSTIC_WARNING:
				result.severity = AgentDiagnosticSeverity::Warning;
				break;
			case AE_GAMEPLAY_DIAGNOSTIC_ERROR:
				result.severity = AgentDiagnosticSeverity::Error;
				break;
			default:
				return Invalid("unknown gameplay diagnostic severity");
		}
		const auto copy = [](AetherGameplayUtf8 view)
		{
			return view.length == 0 ? std::string{} : std::string(view.data, view.length);
		};
		result.code = copy(diagnostic.code);
		result.message = copy(diagnostic.message);
		if (diagnostic.file.length != 0 || diagnostic.line != 0 || diagnostic.column != 0)
		{
			AgentSourceLocation source;
			source.file = copy(diagnostic.file);
			if (diagnostic.line != 0)
			{
				source.line = diagnostic.line;
			}
			if (diagnostic.column != 0)
			{
				source.column = diagnostic.column;
			}
			result.source = std::move(source);
		}
		AE_TRY_VOID(ValidateDiagnostic(result));
		return result;
	}

	Expected<nlohmann::json> SerializeAgentReport(const AgentReport& report)
	{
		if (StatusName(report.status).empty())
		{
			return Invalid("unknown status");
		}
		if (!HasText(report.jobId) || !HasText(report.phase) || !HasText(report.target) || !HasText(report.language) || !HasText(report.profile))
		{
			return Invalid("jobId, phase, target, language and profile must contain text");
		}
		if (report.durationMs < 0)
		{
			return Invalid("durationMs must be nonnegative");
		}

		nlohmann::json diagnostics = nlohmann::json::array();
		bool hasErrors = false;
		for (const auto& diagnostic: report.diagnostics)
		{
			AE_TRY_VOID(ValidateDiagnostic(diagnostic));
			const auto severity = SeverityName(diagnostic.severity);
			hasErrors |= diagnostic.severity == AgentDiagnosticSeverity::Error;
			nlohmann::json item{{"code", diagnostic.code}, {"severity", severity}, {"message", diagnostic.message}};
			if (diagnostic.source)
			{
				const auto& source = *diagnostic.source;
				nlohmann::json location{{"file", source.file}};
				if (source.line)
				{
					location["line"] = *source.line;
				}
				if (source.column)
				{
					location["column"] = *source.column;
				}
				item["source"] = std::move(location);
			}
			diagnostics.push_back(std::move(item));
		}
		if ((report.status == AgentJobStatus::Passed && hasErrors) || (report.status == AgentJobStatus::Failed && !hasErrors))
		{
			return Invalid("passed cannot contain errors; failed requires an error diagnostic");
		}

		nlohmann::json checks = nlohmann::json::array();
		std::unordered_set<std::string> checkNames;
		for (const auto& check: report.requiredChecks)
		{
			if (!HasText(check.name) || StatusName(check.status).empty() || !checkNames.insert(check.name).second)
			{
				return Invalid("required checks need unique nonempty names and valid statuses");
			}
			if (report.status == AgentJobStatus::Passed && check.status != AgentJobStatus::Passed)
			{
				return Invalid("passed requires every required check to have passed");
			}
			checks.push_back({{"name", check.name}, {"status", StatusName(check.status)}});
		}

		nlohmann::json toolchains = nlohmann::json::array();
		for (const auto& toolchain: report.toolchains)
		{
			if (!HasText(toolchain.name) || !HasText(toolchain.version) || !HasText(toolchain.target))
			{
				return Invalid("toolchains require name, version and target");
			}
			toolchains.push_back({{"name", toolchain.name}, {"version", toolchain.version}, {"target", toolchain.target}});
		}
		nlohmann::json artifacts = nlohmann::json::array();
		for (const auto& artifact: report.artifacts)
		{
			if (!HasText(artifact.path) || (artifact.hash && !HasText(*artifact.hash)))
			{
				return Invalid("artifacts require a path and nonempty hash when present");
			}
			nlohmann::json item{{"path", artifact.path}};
			if (artifact.bytes)
			{
				item["bytes"] = *artifact.bytes;
			}
			if (artifact.hash)
			{
				item["hash"] = *artifact.hash;
			}
			artifacts.push_back(std::move(item));
		}

		nlohmann::json exitCode = nullptr;
		switch (report.status)
		{
			case AgentJobStatus::Passed:
				exitCode = 0;
				break;
			case AgentJobStatus::Failed:
				exitCode = 1;
				break;
			case AgentJobStatus::Cancelled:
				exitCode = 130;
				break;
			case AgentJobStatus::Skipped:
				exitCode = 3;
				break;
			case AgentJobStatus::Queued:
			case AgentJobStatus::Running:
				break;
		}
		nlohmann::json result{{"schemaVersion", 1},
		        {"jobId", report.jobId},
		        {"status", StatusName(report.status)},
		        {"phase", report.phase},
		        {"target", report.target},
		        {"language", report.language},
		        {"profile", report.profile},
		        {"diagnostics", std::move(diagnostics)},
		        {"toolchains", std::move(toolchains)},
		        {"artifacts", std::move(artifacts)},
		        {"requiredChecks", std::move(checks)},
		        {"durationMs", report.durationMs},
		        {"exitCode", std::move(exitCode)}};
		try
		{
			// Validate every string, including nested provenance, before the caller dumps JSON.
			(void) result.dump();
		}
		catch (const nlohmann::json::type_error&)
		{
			return Invalid("all strings must be valid UTF-8");
		}
		return result;
	}
} // namespace aether::scripting
