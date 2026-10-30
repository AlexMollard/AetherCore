#include "scene/SceneStreamingDependencies.hpp"

#include <unordered_set>
#include <utility>

#include "scene/SceneSerializer.hpp"

namespace aether::app::scene
{
	namespace
	{
		std::uint64_t HashIdentity(std::string_view region, std::string_view identity)
		{
			std::uint64_t hash = 14695981039346656037ull;
			for (const unsigned char c: region)
			{
				hash ^= c;
				hash *= 1099511628211ull;
			}
			hash ^= 0xff;
			hash *= 1099511628211ull;
			for (const unsigned char c: identity)
			{
				hash ^= c;
				hash *= 1099511628211ull;
			}
			return hash ? hash : 1;
		}
	} // namespace

	std::uint64_t StreamingNodeId(std::string_view region, std::uint64_t source) noexcept
	{
		char digits[20];
		char* end = digits + sizeof(digits);
		char* cursor = end;
		do
		{
			*--cursor = static_cast<char>('0' + source % 10);
			source /= 10;
		}
		while (source);
		std::uint64_t hash = 14695981039346656037ull;
		for (const unsigned char c: region)
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}
		hash ^= 0xff;
		hash *= 1099511628211ull;
		for (const unsigned char c: std::string_view("node:"))
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}
		while (cursor != end)
		{
			hash ^= static_cast<unsigned char>(*cursor++);
			hash *= 1099511628211ull;
		}
		return hash ? hash : 1;
	}

	std::expected<void, std::string> EnsureStreamingSceneNodeIds(SceneDescription& scene)
	{
		std::unordered_set<std::uint64_t> used;
		for (auto& entity: scene.entities)
		{
			auto id = entity.nodeId;
			if (!id)
			{
				std::string identity = "legacy:";
				const EntityRecord* current = &entity;
				std::size_t depth = 0;
				while (current)
				{
					if (current->name.empty())
					{
						return std::unexpected("Legacy entity has no stable identity; assign and save scene node IDs");
					}
					identity += std::to_string(current->name.size()) + ":" + current->name + ":" + std::to_string(current->guid) + "/";
					if (++depth > 32)
					{
						return std::unexpected("Legacy entity hierarchy exceeds depth limit");
					}
					if (current->parentIndex < 0)
					{
						break;
					}
					if (static_cast<std::size_t>(current->parentIndex) >= scene.entities.size())
					{
						return std::unexpected("Invalid legacy entity parent");
					}
					current = &scene.entities[static_cast<std::size_t>(current->parentIndex)];
				}
				id = HashIdentity("legacy", identity);
			}
			if (!used.insert(id).second)
			{
				return std::unexpected("Ambiguous region node identity; assign unique scene node IDs and save the document");
			}
			entity.nodeId = id;
			if (!entity.sourceNodeId)
			{
				entity.sourceNodeId = id;
			}
		}
		return {};
	}

	Expected<Entity> ResolveSceneNodeReference(World& world, std::uint64_t id)
	{
		Entity exact{}, alias{};
		bool duplicateExact = false, duplicateAlias = false;
		if (!id)
		{
			return Entity{};
		}
		world.View<SceneNodeComponent>().each(
		        [&](entt::entity handle, const SceneNodeComponent& node)
		        {
			        if (node.id == id)
			        {
				        if (exact.IsValid())
				        {
					        duplicateExact = true;
				        }
				        else
				        {
					        exact = World::FromEntt(handle);
				        }
			        }
			        if (node.sourceId == id)
			        {
				        if (alias.IsValid())
				        {
					        duplicateAlias = true;
				        }
				        else
				        {
					        alias = World::FromEntt(handle);
				        }
			        }
		        });
		if (duplicateExact)
		{
			return std::unexpected(AetherError::Engine("Ambiguous scene node identity"));
		}
		if (exact.IsValid())
		{
			return exact;
		}
		if (duplicateAlias)
		{
			return std::unexpected(AetherError::Engine("Ambiguous source scene node reference"));
		}
		return alias;
	}

	std::expected<void, std::string> CollectSceneStreamingDependencies(const SceneDescription& scene, std::vector<std::string>& out, const SceneDependencyResolver& resolve)
	{
		std::unordered_set<std::string> seen;
		auto add = [&](const std::string& ref) -> bool
		{
			if (ref.empty())
			{
				return true;
			}
			if (!resolve)
			{
				return false;
			}
			auto stable = resolve(ref);
			if (stable.empty())
			{
				return false;
			}
			if (seen.insert(stable).second)
			{
				out.push_back(std::move(stable));
			}
			return true;
		};
		for (const auto& entity: scene.entities)
		{
			if (entity.mesh && entity.mesh->kind == MeshSourceComponent::Kind::Model && !add(entity.mesh->path))
			{
				return std::unexpected("Invalid model reference");
			}
			if (entity.physics && !add(entity.physics->meshSource))
			{
				return std::unexpected("Invalid collider model reference");
			}
			if (entity.sprite && (!add(entity.sprite->texturePath) || !add(entity.sprite->atlasPath)))
			{
				return std::unexpected("Invalid sprite reference");
			}
			if (entity.spriteAnimator && !add(entity.spriteAnimator->animationPath))
			{
				return std::unexpected("Invalid animation reference");
			}
			if (entity.uiImage && !add(entity.uiImage->texturePath))
			{
				return std::unexpected("Invalid UI image reference");
			}
			if (entity.material)
			{
				const auto& m = *entity.material;
				if (!add(m.assetPath) || !add(m.albedoPath) || !add(m.normalPath) || !add(m.metallicRoughnessPath) || !add(m.occlusionPath) || !add(m.emissivePath))
				{
					return std::unexpected("Invalid material reference");
				}
			}
			for (const auto& component: entity.reflected)
			{
				for (const auto& [name, value]: component.fields)
				{
					const bool resource = (component.type == "Tile Map" && name == "tilemap") || (component.type == "Particle Emitter" && name == "texture") || (component.type == "Audio Source" && name == "clipPath")
					                      || (component.type == "UI Text" && name == "font") || (component.type == "Grass" && (name == "surface_mesh" || name == "exclusion_mesh"));
					if (resource && !value.str.empty() && (value.str.contains("://") || component.type != "UI Text") && !add(value.str))
					{
						return std::unexpected("Invalid reflected asset reference");
					}
				}
			}
		}
		return {};
	}
} // namespace aether::app::scene
