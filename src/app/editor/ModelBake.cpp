#include "editor/ModelBake.hpp"

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>

#include "MeshProcessor.hpp"

#include "assets/GltfAsset.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace fs = std::filesystem;

	namespace
	{
		// Drop the "<mount>://" prefix, leaving a project-relative path.
		std::string StripMount(const std::string& vfs)
		{
			const auto ss = vfs.find("://");
			return ss == std::string::npos ? vfs : vfs.substr(ss + 3);
		}

		// Write baked bytes straight to disk under the project root (project:// is
		// not guaranteed writable through the VFS, but the raw project folder is).
		bool WriteBaked(const fs::path& outPath, const assetpipeline::ByteBuffer& data, std::string& error)
		{
			std::error_code ec;
			fs::create_directories(outPath.parent_path(), ec);
			std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				error = "cannot open '" + outPath.generic_string() + "' for write";
				return false;
			}
			out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
			if (!out)
			{
				error = "failed writing '" + outPath.generic_string() + "'";
				return false;
			}
			return true;
		}
	} // namespace

	bool EnsureModelBaked(const std::string& vfsModelPath, const app::EditorProjectContext& project, std::string& error)
	{
		// Already baked? The loader resolves a .mesh sibling through the VFS; if it's
		// there (raw folder or pak), we're done. Same check the loader uses.
		const std::string meshVfs = assets::GltfAsset::ResolveMeshPath(vfsModelPath);
		if (!meshVfs.empty() && io::FileSystem::Exists(meshVfs))
		{
			return true;
		}

		if (!project.IsLoaded())
		{
			error = "no project loaded";
			return false;
		}

		// Read the raw glTF/GLB through the VFS.
		const auto raw = io::FileSystem::ReadFile(vfsModelPath);
		if (!raw)
		{
			error = "cannot read model '" + vfsModelPath + "'";
			return false;
		}

		const std::string rel = StripMount(vfsModelPath); // assets/models/Fox/Fox.gltf
		const fs::path diskPath = project.root / rel;      // disk source (resolves external .bin URIs)

		const auto result = assetpipeline::MeshProcessor::Process(
		        std::span<const std::byte>(raw->data(), raw->size()), diskPath, rel, project.root);

		if (result.meshData.empty())
		{
			error = "no mesh geometry produced from '" + vfsModelPath + "' (animation-only or unsupported glTF)";
			return false;
		}

		// Mirror AssetProcessor's pak layout, but under the project root on disk.
		// These files then resolve immediately and get packed as-is on Publish.
		const std::string stem = fs::path(rel).stem().generic_string();
		const fs::path modelDir = (project.root / rel).parent_path();

		if (!WriteBaked(modelDir / (stem + ".mesh"), result.meshData, error))
		{
			return false;
		}
		if (!result.skelData.empty() && !WriteBaked(modelDir / (stem + ".skel"), result.skelData, error))
		{
			return false;
		}
		if (!result.animsetData.empty() && !WriteBaked(modelDir / (stem + ".animset"), result.animsetData, error))
		{
			return false;
		}
		for (const auto& [fileName, animData]: result.animFiles)
		{
			if (!animData.empty() && !WriteBaked(project.root / "animations" / fileName, animData, error))
			{
				return false;
			}
		}
		for (const auto& [matVfsPath, matData]: result.materialFiles)
		{
			if (!matData.empty() && !WriteBaked(project.root / matVfsPath, matData, error))
			{
				return false;
			}
		}

		AE_INFO(LogCategory::App, "Imported model '{}' ({} material(s), {} clip(s)) -> {}.mesh", vfsModelPath, result.materialFiles.size(), result.animFiles.size(), stem);
		return true;
	}
} // namespace aether::editor
