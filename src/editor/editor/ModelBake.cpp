#include "editor/ModelBake.hpp"

#include <filesystem>
#include <span>
#include <string>

#include "MeshProcessor.hpp"
#include "BakeOutputs.hpp"

#include "assets/GltfAsset.hpp"
#include "editor/EditorProjectContext.hpp"
#include "io/FileSystem.hpp"
#include "utils/Logger.hpp"

namespace aether::editor
{
	namespace fs = std::filesystem;

	namespace
	{
		std::string StripMount(const std::string& vfs)
		{
			const auto ss = vfs.find("://");
			return ss == std::string::npos ? vfs : vfs.substr(ss + 3);
		}

	} // namespace

	bool EnsureModelBaked(const std::string& vfsModelPath, const app::EditorProjectContext& project, std::string& error)
	{
		if (!project.IsLoaded())
		{
			const std::string meshVfs = assets::GltfAsset::ResolveMeshPath(vfsModelPath);
			if (!meshVfs.empty() && io::FileSystem::Exists(meshVfs))
			{
				return true;
			}
			error = "no project loaded";
			return false;
		}
		return EnsureModelBaked(vfsModelPath, project.root, error);
	}

	bool EnsureModelBaked(const std::string& vfsModelPath, const fs::path& projectRoot, std::string& error)
	{
		const std::string resolved = io::FileSystem::ResolveAssetPath(vfsModelPath);
		const std::string meshVfs = assets::GltfAsset::ResolveMeshPath(resolved);
		const std::string rel = StripMount(resolved);
		const fs::path diskPath = projectRoot / rel;
		const bool meshExists = !meshVfs.empty() && io::FileSystem::Exists(meshVfs);
		if (meshExists && resolved.ends_with(".mesh"))
		{
			return true;
		}
		if (meshExists && !io::FileSystem::Exists(resolved))
		{
			return true; // Cooked-only projects can still consume legacy meshes.
		}
		if (assetpipeline::HasCurrentModelBake(diskPath, projectRoot))
		{
			return true;
		}

		const auto raw = io::FileSystem::ReadFile(resolved);
		if (!raw)
		{
			error = "cannot read model '" + vfsModelPath + "'";
			return false;
		}


		const auto result = assetpipeline::MeshProcessor::Process(std::span<const std::byte>(raw->data(), raw->size()), diskPath, rel, projectRoot);
		if (!result.error.empty())
		{
			error = result.error;
			return false;
		}

		if (result.meshData.empty())
		{
			error = "no mesh geometry produced from '" + vfsModelPath + "' (animation-only or unsupported glTF)";
			return false;
		}

		const auto write = assetpipeline::WriteBakedOutputs(result, diskPath, projectRoot);
		if (!write.ok)
		{
			error = write.error;
			return false;
		}
		const std::string stem = diskPath.stem().generic_string();

		AE_INFO(LogCategory::App, "Imported model '{}' ({} material(s), {} clip(s)) -> {}.mesh", vfsModelPath, result.materialFiles.size(), result.animFiles.size(), stem);
		return true;
	}
} // namespace aether::editor
