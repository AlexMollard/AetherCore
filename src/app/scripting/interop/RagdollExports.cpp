#include "scripting/interop/InteropCommon.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "assets/GltfAsset.hpp"
#include "physics/PhysicsComponents.hpp"
#include "physics/RagdollBuilder.hpp"
#include "scene/World.hpp"
#include "utils/LogCategory.hpp"
#include "utils/Logger.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

namespace
{
	// Per-thread: an inbound RPC dispatched on a worker thread (or any second caller
	// reaching this export between Spawn() and the BoneAt() getter) must not rebind
	// the result set another script is still indexing - same reasoning as
	// PhysicsExports.cpp's overlap cache.
	thread_local std::vector<std::uint32_t> g_ragdollBonesCache;
} // namespace

// Ragdoll.Spawn's only native entry point - see RagdollBuilder.hpp for exactly what it
// builds. `skeletonPathC` is read purely for its bind-pose skeleton (bone names,
// parents, positions); nothing about rendering or animation is touched, so this loads
// it directly and synchronously rather than going through AssetManager's async model
// pipeline - a ragdoll spawn is a rare, one-shot event, not a hot path, and it needs
// none of what that pipeline is for (GPU upload, draw-ready meshes, ...).
//
// Returns the number of bones (root included) on success, 0 on failure, and stashes
// their ids in g_ragdollBonesCache for aether_ragdoll_spawn_bone_at to hand back one
// at a time - the same count-then-index shape PhysicsExports.cpp's OverlapSphere
// uses, so a caller never needs to guess a buffer size for what is always a small,
// bounded set (kRagdollBoneDefs tops out at eleven). The list is read straight off
// the RagdollComponent SpawnRagdoll itself just populated on the root entity, so it
// can never disagree with what was actually created - unlike the before/after
// entity-set diff a caller had to do without this.
AE_SCRIPT_API std::int32_t aether_ragdoll_spawn(std::uint32_t id, const char* skeletonPathC)
{
	return SafeExport([&] -> std::int32_t
	{
	g_ragdollBonesCache.clear();
	if (!EntityAlive(id))
	{
		return 0;
	}
	const std::string skeletonPath = skeletonPathC != nullptr ? skeletonPathC : "";
	auto skeleton = aether::assets::GltfAsset::LoadFromVfsPath(skeletonPath);
	if (!skeleton.has_value())
	{
		AE_WARN(aether::LogCategory::App, "Ragdoll.Spawn: could not load skeleton '{}': {}", skeletonPath, skeleton.error());
		return 0;
	}
	aether::World& world = ActiveWorld();
	const aether::Entity source{id};
	if (!aether::SpawnRagdoll(world, source, *skeleton))
	{
		return 0;
	}
	if (const auto* ragdoll = world.TryGet<aether::RagdollComponent>(source))
	{
		g_ragdollBonesCache.reserve(ragdoll->bones.size());
		for (const aether::Entity bone: ragdoll->bones)
		{
			g_ragdollBonesCache.push_back(bone.id);
		}
	}
	return static_cast<std::int32_t>(g_ragdollBonesCache.size());
	});
}

AE_SCRIPT_API std::uint32_t aether_ragdoll_spawn_bone_at(std::int32_t index)
{
	return SafeExport([&] -> std::uint32_t
	{
	if (index < 0 || static_cast<std::size_t>(index) >= g_ragdollBonesCache.size())
	{
		return 0;
	}
	return g_ragdollBonesCache[static_cast<std::size_t>(index)];
	});
}

// Ragdoll.DriveSkin's only native entry point. Attaches (or replaces)
// RagdollSkinDriveComponent on `meshEntityId`, so WorldRenderer::Flush starts
// substituting `ragdollRootId`'s live bone poses into that mesh's skin every frame
// instead of whatever animation clip it last sampled - see
// rendering/RagdollSkinDrive.hpp for exactly how. Deliberately does not validate that
// ragdollRootId currently carries a RagdollComponent: a ragdoll spawned one frame
// after this call (or destroyed later) is not an error here, only a frame that
// renders the mesh's last sampled animation pose instead of a driven one - see
// BuildRagdollSkinOverrides' own empty-result handling.
AE_SCRIPT_API void aether_ragdoll_drive_skin(std::uint32_t meshEntityId, std::uint32_t ragdollRootId)
{
	SafeExport([&] -> void
	{
	if (!EntityAlive(meshEntityId))
	{
		return;
	}
	ActiveWorld().EmplaceOrReplace<aether::RagdollSkinDriveComponent>(aether::Entity{meshEntityId}, aether::RagdollSkinDriveComponent{.ragdollRoot = aether::Entity{ragdollRootId}});
	});
}
