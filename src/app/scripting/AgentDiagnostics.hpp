#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "utils/Expected.hpp"
#include "scripting/GameplayAbi.h"

namespace aether::scripting
{
	enum class AgentJobStatus
	{
		Queued,
		Running,
		Passed,
		Failed,
		Cancelled,
		Skipped
	};

	enum class AgentDiagnosticSeverity
	{
		Info,
		Warning,
		Error
	};

	struct AgentSourceLocation
	{
		std::string file;
		std::optional<std::uint32_t> line;
		std::optional<std::uint32_t> column;
	};

	struct AgentDiagnostic
	{
		std::string code;
		AgentDiagnosticSeverity severity = AgentDiagnosticSeverity::Info;
		std::string message;
		std::optional<AgentSourceLocation> source;
	};

	struct AgentToolchain
	{
		std::string name;
		std::string version;
		std::string target;
	};

	struct AgentArtifact
	{
		std::string path;
		std::optional<std::uint64_t> bytes;
		std::optional<std::string> hash;
	};

	struct AgentRequiredCheck
	{
		std::string name;
		AgentJobStatus status = AgentJobStatus::Queued;
	};

	// An operation snapshot, independent of the editor, script language and transport.
	// Queued/running snapshots have no exit code. Only a validated passed snapshot means success.
	struct AgentReport
	{
		std::string jobId;
		AgentJobStatus status = AgentJobStatus::Queued;
		std::string phase;
		std::string target;
		std::string language;
		std::string profile;
		std::vector<AgentDiagnostic> diagnostics;
		std::vector<AgentToolchain> toolchains;
		std::vector<AgentArtifact> artifacts;
		std::vector<AgentRequiredCheck> requiredChecks;
		std::int64_t durationMs = 0;
	};

	// Pure. Rejects invalid metadata and contradictory success before returning JSON.
	[[nodiscard]] Expected<nlohmann::json> SerializeAgentReport(const AgentReport& report);

	// Copies a borrowed v1 ABI diagnostic synchronously. ABI table version negotiation
	// precedes this call; the diagnostic itself carries only its exact v1 struct size.
	[[nodiscard]] Expected<AgentDiagnostic> FromGameplayDiagnostic(const AetherGameplayDiagnostic& diagnostic);
} // namespace aether::scripting
