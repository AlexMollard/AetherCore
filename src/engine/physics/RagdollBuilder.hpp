#pragma once

#include "scene/Entity.hpp"

namespace aether::assets
{
	struct GltfAsset;
}

namespace aether
{
	class World;

	// Builds a physics-driven ragdoll from a humanoid skeleton's bind pose: roughly a
	// dozen dynamic capsule bodies (see RagdollBuilder.cpp's kRagdollConventions) wired
	// together with Hinge (elbows, knees) and Swing Twist (shoulders, hips, neck, spine)
	// joints, matched to `skeleton` by case-insensitive bone-name matching against
	// EACH known naming convention in turn (Mixamo, Quaternius, ...) - see
	// kRagdollConventions' own comment for exactly what that does and does not cover,
	// and why a rig matching neither convention in full fails outright with a logged
	// reason rather than silently building a partial ragdoll.
	//
	// v1 renders each bone as its own primitive box/sphere mesh sized to the capsule -
	// there is no capsule primitive mesh in the engine's built-in set, and no GPU path
	// yet exists to drive a skinned mesh's real bones from physics. That GPU path (see
	// kMixamoBoneDefs' own file comment for the exact seam) is deliberately not part of
	// this: this is a physics-only feature. Spawning, colliding with the world and other
	// bodies, and being picked up by the physics gun already work through nothing but
	// RigidBodyComponent/ColliderComponent, which every other system already understands
	// - a ragdoll bone needs no special-casing anywhere else in the engine.
	//
	// `source` becomes the ragdoll's root (pelvis) bone, keeping its entity id - and
	// everything else already on it (NetworkIdentity, scripts, ...) - intact through the
	// transition. It must already have a TransformComponent (the ragdoll's drop pose and
	// facing); this function does not invent one. If `source` already carries a
	// CharacterControllerComponent (a player being knocked out or killed), its current
	// velocity seeds the root bone's initial velocity and the component is removed - a
	// Rigid Body/Collider and a Character Controller never coexist on one entity (see
	// PhysicsSystem's own conflict check).
	//
	// Returns false, changing nothing, if `source` has no TransformComponent, is already
	// a ragdoll bone, or `skeleton` has no bind-pose node recognisable as a Hips-
	// equivalent role - there is nothing sensible to build.
	bool SpawnRagdoll(World& world, Entity source, const assets::GltfAsset& skeleton);
} // namespace aether
