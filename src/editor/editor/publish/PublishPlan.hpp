#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::app
{
	struct EditorProjectContext;
}

namespace aether::editor
{
	// Everything about the machine and build doing the publishing. Separated from the
	// resolution below so the resolution is pure and can be tested without a real editor.
	struct PublishEnvironment
	{
		std::filesystem::path bundleDir;  // directory the running editor lives in
		std::string runtimeExeName;       // e.g. "AetherGame.exe"
		std::string platformName;         // e.g. "Windows"
		std::string configName;           // e.g. "Debug" - AE_CONFIG_NAME
		bool shippableConfig = false;     // false for Debug/RelWithDebInfo
	};

	// Every path and label a publish needs, resolved once up front. Nothing here is
	// configurable: the output location is derived from the project, which is what makes
	// "one button, no options" possible.
	struct PublishPlan
	{
		std::filesystem::path projectRoot;
		std::filesystem::path projectFile;
		std::filesystem::path scenesDir;
		std::filesystem::path scriptsDir;
		std::filesystem::path bundleDir;
		std::filesystem::path outputDir;
		std::string productName;
		std::string platformName;
		std::string runtimeExeName;
		std::string configName;
		bool shippableConfig = false;
	};

	// Replaces a path segment's illegal characters with '_' and trims the trailing dots
	// and spaces Windows silently drops. Returns `fallback` when nothing usable remains.
	[[nodiscard]] std::string SanitizePublishSegment(std::string value, std::string_view fallback);

	// Pure. No filesystem access - it only composes paths.
	[[nodiscard]] PublishPlan MakePublishPlan(const app::EditorProjectContext& project, const PublishEnvironment& environment);

	// Reads the running executable's directory and the compile-time build config.
	[[nodiscard]] PublishEnvironment CurrentPublishEnvironment();
} // namespace aether::editor
