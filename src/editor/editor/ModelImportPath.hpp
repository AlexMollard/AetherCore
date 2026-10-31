#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace aether::editor
{
	// Normalize before any VFS operation: an absolute OS path is not a VFS URI.
	inline bool NormalizeModelImportPath(std::string_view input, const std::filesystem::path& projectRoot, std::string& normalized, std::string& error)
	{
		namespace fs = std::filesystem;
		const bool projectUri = input.starts_with("project://");
		const auto path = fs::u8path(projectUri ? input.substr(10) : input);
		if (!projectUri && input.find("://") != std::string_view::npos)
		{
			normalized = input;
			return true;
		}
		if (projectRoot.empty() || (!projectUri && !path.is_absolute()) || (projectUri && path.is_absolute()))
		{
			error = "Use project:// or an absolute model path inside the open project";
			return false;
		}
		std::error_code ec;
		const auto root = fs::weakly_canonical(projectRoot, ec);
		if (ec)
		{
			error = "Cannot resolve the open project directory: " + ec.message();
			return false;
		}
		const auto diskPath = fs::weakly_canonical(projectUri ? root / path : path, ec);
		if (ec)
		{
			error = "Cannot resolve the model path: " + ec.message();
			return false;
		}
		const auto relative = diskPath.lexically_relative(root);
		if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..")
		{
			error = "Model path is outside the open project; copy/import the model and its companion files into the project assets directory first";
			return false;
		}
		const auto utf8 = relative.generic_u8string();
		normalized = "project://" + std::string(utf8.begin(), utf8.end());
		return true;
	}
} // namespace aether::editor
