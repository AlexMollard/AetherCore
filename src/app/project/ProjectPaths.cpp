#include "project/ProjectPaths.hpp"

#include <cctype>

namespace aether::app::project
{
	std::string SanitizeProjectFolderName(std::string_view name)
	{
		std::string out;
		out.reserve(name.size());
		bool pendingSpace = false;
		for (const char c: name)
		{
			const unsigned char uc = static_cast<unsigned char>(c);
			// Whitespace (space, tab, newline, ...) is a separator: collapse to a
			// single space. Check before iscntrl since tab/newline are both.
			if (std::isspace(uc) != 0)
			{
				pendingSpace = !out.empty(); // drop leading whitespace
				continue;
			}
			if (std::iscntrl(uc) != 0)
			{
				continue;
			}
			if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
			{
				continue;
			}
			if (pendingSpace)
			{
				out.push_back(' ');
				pendingSpace = false;
			}
			out.push_back(c);
		}
		return out; // pendingSpace never flushed at the end -> trailing space dropped
	}

	std::filesystem::path ComposeNewProjectRoot(const std::filesystem::path& parent, std::string_view name)
	{
		const std::string folder = SanitizeProjectFolderName(name);
		if (parent.empty() || folder.empty())
		{
			return {};
		}
		return (parent / folder).lexically_normal();
	}

	std::filesystem::path DefaultNewProjectParent(std::span<const std::filesystem::path> recentRoots, const std::filesystem::path& documentsDir)
	{
		for (const std::filesystem::path& root: recentRoots)
		{
			if (root.empty())
			{
				continue;
			}
			// Skip entries whose folder has since been moved or deleted: proposing a path
			// that no longer exists is worse than proposing nothing.
			const std::filesystem::path parent = root.parent_path();
			std::error_code ec;
			if (!parent.empty() && std::filesystem::is_directory(parent, ec))
			{
				return parent.lexically_normal();
			}
		}

		if (!documentsDir.empty())
		{
			return (documentsDir / "AetherCore Projects").lexically_normal();
		}
		return {};
	}
} // namespace aether::app::project
