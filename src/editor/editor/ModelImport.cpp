#include "editor/ModelImport.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/AssetManager.hpp"
#include "editor/EditorProjectContext.hpp"
#include "editor/ModelBake.hpp"
#include "scene/Components.hpp"
#include "scene/ModelSpawn.hpp"
#include "scene/World.hpp"
#include "scripting/SceneContext.hpp"
#include "utils/Logger.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::editor
{
	Entity ImportModelIntoScene(World& world, ServiceContainer& services, const std::string& vfsModelPath, const glm::mat4& localToWorld, const std::string& name, std::string& error)
	{
		auto* assets = services.TryGet<AssetManager>();
		auto* sceneCtx = services.TryGet<app::scripting::SceneContext>();
		if (assets == nullptr)
		{
			error = "no AssetManager";
			return {};
		}
		if (sceneCtx == nullptr)
		{
			error = "no SceneContext";
			return {};
		}

		if (const auto* project = services.TryGet<app::EditorProjectContext>(); project != nullptr && project->IsLoaded())
		{
			if (!EnsureModelBaked(vfsModelPath, *project, error))
			{
				return {};
			}
		}

		const Entity root = world.Create();
		if (!name.empty())
		{
			world.Emplace<NameComponent>(root, NameComponent{.name = name});
		}
		world.Emplace<TransformComponent>(root, TransformComponent{.localToWorld = localToWorld});

		// Unity-style layout: single static mesh directly on `root`, multi-primitive
		if (!app::scene::AssignModelToEntity(world, *assets, *sceneCtx, root, vfsModelPath))
		{
			world.Destroy(root);
			if (error.empty())
			{
				error = "model load failed: " + vfsModelPath;
			}
			return {};
		}

		if (auto* db = services.TryGet<AssetDatabase>())
		{
			app::scene::RegisterModelAssets(*db, *assets, *sceneCtx, vfsModelPath);
		}
		AE_INFO(LogCategory::App, "Imported model '{}' into the scene (entity {})", vfsModelPath, root.id);
		return root;
	}
} // namespace aether::editor
