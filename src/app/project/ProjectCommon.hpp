#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditorProjectContext.hpp"
#include "utils/Expected.hpp"

namespace aether
{
	class TomlConfig;
} // namespace aether

namespace aether::app::project
{
	// Engine-agnostic project helpers shared by the editor's project manager and the
	// standalone Launcher. Nothing here depends on the editor's scene, panels, or
	// publish/shader code, so the slim Launcher target links it without pulling the
	// editor in. This is the home of everything both front ends need to enumerate,
	// read, and scaffold projects on disk.

	inline constexpr int kMaxRecentProjects = 8;

	// --- Path / descriptor -------------------------------------------------------
	[[nodiscard]] std::filesystem::path NormalizePath(std::filesystem::path path);
	[[nodiscard]] std::string DisplayPath(const std::filesystem::path& path);
	[[nodiscard]] std::filesystem::path ProjectFilePath(const std::filesystem::path& root);
	// Accepts either a project root or a ProjectSettings.toml path and returns the root.
	[[nodiscard]] std::filesystem::path ResolveProjectRoot(std::filesystem::path path);
	[[nodiscard]] bool HasProjectDescriptor(const std::filesystem::path& root);
	[[nodiscard]] std::string FallbackProjectName(const std::filesystem::path& root);
	// The per-project preview thumbnail the launcher shows for a project. Written by
	// the editor on scene save (a capture of the scene viewport). Lives under a hidden
	// per-project metadata dir so it never pollutes the asset tree.
	[[nodiscard]] std::filesystem::path PreviewImagePath(const std::filesystem::path& root);
	// A short human label for when the project was last touched - the newest write time
	// of its ProjectSettings.toml and its preview thumbnail (the editor rewrites the
	// preview on every scene save, so this tracks editing): "just now", "5 min ago",
	// "3 days ago", or an absolute date for older projects. Empty when nothing readable
	// exists (e.g. a project whose folder is gone).
	[[nodiscard]] std::string LastModifiedLabel(const std::filesystem::path& root);
	// Reads ProjectSettings.toml at `root` into a fully-resolved context. Falls back to
	// folder-derived defaults for any missing field; errors only when no descriptor exists.
	[[nodiscard]] Expected<EditorProjectContext> ReadProjectDescriptor(const std::filesystem::path& root);
	[[nodiscard]] std::string ReadProjectName(const std::filesystem::path& root);

	// --- Create project ----------------------------------------------------------
	// Scaffolds a new project at `root`: standard folders + ProjectSettings.toml +
	// scripts/AetherGame.csproj + a seed scene. Returns false and sets `error` on failure.
	[[nodiscard]] bool WriteProjectDescriptor(const std::filesystem::path& root, std::string_view name, std::string& error);
	// The project game-scripts .csproj text (an absolute ProjectReference to the engine SDK).
	[[nodiscard]] std::string MakeProjectScriptCsprojText(const std::filesystem::path& managedSdkProject);

	// --- Native pickers (Win32; std::nullopt on other platforms or on cancel) ----
	[[nodiscard]] std::optional<std::filesystem::path> PickProjectFolder();
	[[nodiscard]] std::optional<std::filesystem::path> PickProjectFile();

	// --- Recent-projects persistence (TomlConfig `launcher.recent_*` keys) --------
	// Reads up to kMaxRecentProjects entries, de-duplicating by normalized root and
	// back-filling missing names from each project's descriptor.
	[[nodiscard]] std::vector<EditorProjectContext> LoadRecentProjects(TomlConfig& config);
	// Writes the list back (padding unused slots with empty strings so stale entries clear).
	void SaveRecentProjects(TomlConfig& config, std::span<const EditorProjectContext> recents);
} // namespace aether::app::project
