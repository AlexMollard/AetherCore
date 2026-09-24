#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aether::editor
{
	// What broke, and what to do about it. Kept separate so the UI can show the fix without
	// the user having to infer it from the failure text.
	struct PublishIssue
	{
		std::string message;
		std::string remediation;
	};

	// Build leftovers that are safe to delete from a package (the prune step removes them).
	[[nodiscard]] bool IsPrunablePublishedFile(const std::filesystem::path& path);

	// Source and project files that must never ship (verification rejects them).
	[[nodiscard]] bool IsForbiddenPublishedFile(const std::filesystem::path& path);

	// Aftermath is dev-only and editor-gated; a shipped runtime never loads it.
	[[nodiscard]] bool IsAftermathRuntimeFile(const std::filesystem::path& path);

	// Packer sidecars have no runtime purpose and leak dev paths.
	[[nodiscard]] bool IsPakSidecarFile(const std::filesystem::path& path);

	// Every invariant a published package must satisfy. Returns nullopt when the package is
	// good. This always runs - a package that fails it is a failed publish, not a warning.
	[[nodiscard]] std::optional<PublishIssue> VerifyPublishedPackage(const std::filesystem::path& packageDir, std::string_view runtimeExeName);
} // namespace aether::editor
