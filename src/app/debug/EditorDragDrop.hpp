#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace aether::editor::dragdrop
{
	inline constexpr const char* kEntityPayload = "AETHER_ENTITY";
	inline constexpr const char* kScriptPayload = "AETHER_SCRIPT_TYPE";
	inline constexpr const char* kFilePayload = "AETHER_FILE_PATH";

	enum class FileKind : std::uint32_t
	{
		Unknown,
		Model,
		Material,
		MaterialGraph,
		Texture,
		Script,
		Prefab,
		Scene,
		Shader,
	};

	struct ScriptPayload
	{
		char typeName[128] = {};
		char sourcePath[260] = {};
	};

	// The one place a file's extension decides what it is.
	//
	// This lived in three: the file explorer, the asset.select control method, and the
	// inspector's idea of what it was looking at. They disagreed about materials, so a
	// .material.toml selected from a script showed up as a plain File and got no material
	// UI at all, while the same file clicked in the browser did.
	[[nodiscard]] inline FileKind ClassifyFile(const std::filesystem::path& path)
	{
		std::string ext = path.extension().generic_string();
		for (char& c: ext)
		{
			c = static_cast<char>((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
		}
		if (ext == ".mesh" || ext == ".gltf" || ext == ".glb")
		{
			return FileKind::Model;
		}
		if (ext == ".cs")
		{
			return FileKind::Script;
		}
		if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".dds" || ext == ".texture")
		{
			return FileKind::Texture;
		}
		if (ext == ".slang")
		{
			return FileKind::Shader;
		}
		if (ext == ".material")
		{
			return FileKind::Material;
		}
		if (ext == ".toml")
		{
			const std::string generic = path.generic_string();
			// A material GRAPH is not a material: it generates one. Checked before the
			// material suffix and before the folder heuristic below, or the Inspector renders
			// a graph file as a set of material properties and parses a graph as a material.
			if (generic.contains(".materialgraph.toml"))
			{
				return FileKind::MaterialGraph;
			}
			if (generic.contains(".material.toml"))
			{
				return FileKind::Material;
			}
			if (generic.contains(".prefab.toml"))
			{
				return FileKind::Prefab;
			}
			if (generic.contains(".scene.toml"))
			{
				return FileKind::Scene;
			}
			// A model import writes its materials into a materials/ folder beside the mesh,
			// under names that carry no suffix of their own.
			if (generic.contains("/materials/") || path.filename() == "properties.toml")
			{
				return FileKind::Material;
			}
		}
		return FileKind::Unknown;
	}

	struct FilePayload
	{
		FileKind kind = FileKind::Unknown;
		char path[260] = {};
		char displayName[128] = {};
	};

	struct EntityPayload
	{
		std::uint32_t id = 0;
	};
} // namespace aether::editor::dragdrop
