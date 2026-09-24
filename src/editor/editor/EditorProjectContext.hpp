#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::app
{
	enum class ProjectKind
	{
		Scene3D,
		Scene2D,
	};

	struct EditorProjectContext
	{
		std::filesystem::path root;
		std::filesystem::path projectFile;
		std::filesystem::path assetsDir;
		std::filesystem::path scenesDir;
		std::filesystem::path prefabsDir;
		std::filesystem::path scriptsDir;
		std::string name;
		// Editor workbench flavor from ProjectSettings.toml ("editor.flavor"). Empty means
		// the default editor; a recognized flavor (e.g. "twinsanity") gates which flavor
		// panels register and which editor theme applies. See docs/twinsanity-editor.md.
		std::string editorFlavor;
		ProjectKind kind = ProjectKind::Scene3D;
		bool loaded = false;

		[[nodiscard]] bool IsLoaded() const noexcept
		{
			return loaded && !root.empty();
		}
	};

	// Resolve a project:// virtual path (or a bare relative path) against the project
	// root. Returns {} when no project is open. Shared so panels and control methods
	// resolve project paths identically.
	[[nodiscard]] inline std::filesystem::path ResolveProjectPath(const EditorProjectContext* project, std::string_view vpath)
	{
		if (project == nullptr || project->root.empty())
		{
			return {};
		}
		constexpr std::string_view kPrefix = "project://";
		if (vpath.starts_with(kPrefix))
		{
			vpath.remove_prefix(kPrefix.size());
		}
		return project->root / std::filesystem::path(vpath);
	}
} // namespace aether::app
