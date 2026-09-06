#include "rendering/RagdollSkinDrive.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <glm/gtc/matrix_transform.hpp>

#include "physics/PhysicsComponents.hpp"
#include "scene/Components.hpp"
#include "scene/World.hpp"

namespace aether
{
	namespace
	{
		bool EqualsCI(std::string_view a, std::string_view b)
		{
			return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
		}

		// Matches RagdollBuilder.cpp's own node-name resolution: prefer an exact match
		// on the node's own local name (the part after the last '_'/':' separator),
		// falling back to a plain case-insensitive substring match. A ragdoll bone's
		// skinNodeName is the EXACT glTF node name it was measured from at spawn, so an
		// exact match is the expected common case; the substring fallback only helps a
		// mesh baked from a differently-prefixed export of the same rig.
		std::optional<std::uint32_t> FindNodeIndexByName(std::span<const std::string> nodeNames, std::string_view target)
		{
			std::optional<std::uint32_t> substringMatch;
			for (std::size_t i = 0; i < nodeNames.size(); ++i)
			{
				const std::string_view name = nodeNames[i];
				if (EqualsCI(name, target))
				{
					return static_cast<std::uint32_t>(i);
				}
				if (!substringMatch.has_value())
				{
					const std::size_t sep = name.find_last_of("_:");
					const std::string_view localName = sep == std::string_view::npos ? name : name.substr(sep + 1);
					if (EqualsCI(localName, target))
					{
						substringMatch = static_cast<std::uint32_t>(i);
					}
				}
			}
			return substringMatch;
		}
	} // namespace

	std::vector<AnimationContracts::RagdollOverrideEntry> BuildRagdollSkinOverrides(
	        const World& world, Entity ragdollRoot, std::span<const std::string> targetNodeNames, const glm::mat4& meshWorldToModel)
	{
		std::vector<AnimationContracts::RagdollOverrideEntry> out;
		const auto* ragdoll = world.TryGet<RagdollComponent>(ragdollRoot);
		if (ragdoll == nullptr)
		{
			return out;
		}
		out.reserve(ragdoll->bones.size());
		for (const Entity bone: ragdoll->bones)
		{
			const auto* boneComp = world.TryGet<RagdollBoneComponent>(bone);
			const auto* transform = world.TryGet<TransformComponent>(bone);
			if (boneComp == nullptr || transform == nullptr || boneComp->skinNodeName.empty())
			{
				continue;
			}
			const std::optional<std::uint32_t> nodeIndex = FindNodeIndexByName(targetNodeNames, boneComp->skinNodeName);
			if (!nodeIndex.has_value())
			{
				continue;
			}

			// The body's own CURRENT world transform, position and rotation only -
			// visualScale (the capsule's display size, baked into this same
			// TransformComponent for rendering the debug box/sphere) must never leak
			// into the skinned mesh's joint transform.
			glm::mat3 basis(transform->localToWorld);
			basis[0] = glm::normalize(basis[0]);
			basis[1] = glm::normalize(basis[1]);
			basis[2] = glm::normalize(basis[2]);
			const glm::mat4 bodyWorld = glm::translate(glm::mat4(1.0f), glm::vec3(transform->localToWorld[3])) * glm::mat4(basis);
			const glm::mat4 nodeWorld = bodyWorld * boneComp->skinNodeOffset;

			out.push_back(AnimationContracts::RagdollOverrideEntry{
			        .nodeIndex = *nodeIndex,
			        .transform = meshWorldToModel * nodeWorld,
			});
		}
		return out;
	}
} // namespace aether
