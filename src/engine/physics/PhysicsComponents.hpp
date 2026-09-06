#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	// Designed with networking in mind: static geometry never needs net sync,
	enum class PhysicsLayer : uint8_t
	{
		NonMoving = 0,
		Moving = 1,
		Sensor = 2,
	};

	enum class PhysicsMotionType : uint8_t
	{
		Static, // Immovable; never added to the network sync list
		Kinematic,
		Dynamic,
	};

	struct PhysicsBodyHandle
	{
		static constexpr std::uint32_t kInvalidValue = UINT32_MAX;

		std::uint32_t value = kInvalidValue;

		[[nodiscard]] bool IsValid() const
		{
			return value != kInvalidValue;
		}
	};

	enum class PhysicsShapeType : uint8_t
	{
		Box,
		Sphere,
		Capsule,
		Cylinder,
		// A shape built from an arbitrary mesh's own vertices (see PhysicsSystem's
		// LoadMeshSourceGeometry) - usable on a body of any motion type, unlike Mesh
		// below, since Jolt reduces the input to a closed convex surface.
		ConvexHull,
		// The exact triangles of ColliderComponent::meshSource, for concave geometry a
		// hull cannot represent (e.g. the open space under a table). Jolt requires this
		// shape's body to be Static - PhysicsSystem::FlushPendingBodies rejects it
		// outright (with a warning) on anything else, since Jolt itself does not
		// self-enforce MeshShape::MustBeStatic().
		Mesh,
	};

	struct ColliderComponent
	{
		PhysicsShapeType shape = PhysicsShapeType::Box;
		glm::vec3 halfExtents{0.5f, 0.5f, 0.5f};
		float radius = 0.5f;
		float halfHeight = 0.5f;
		glm::vec3 center{0.0f, 0.0f, 0.0f};

		float friction = 0.5f;
		float restitution = 0.0f;
		bool isSensor = false;
		PhysicsLayer layer = PhysicsLayer::Moving;

		// ConvexHull/Mesh only: a project:// VFS path to the model whose baked vertex
		// positions (and, for Mesh, indices) become the collision shape - the SAME kind
		// of path MeshSourceComponent::path uses for Kind::Model. Ignored by every
		// other shape.
		std::string meshSource;
	};

	struct RigidBodyComponent
	{
		PhysicsBodyHandle body;
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
		float mass = 0.0f;
		float linearDamping = 0.05f;
		float angularDamping = 0.05f;
		float gravityFactor = 1.0f;
		float maxLinearVelocity = 500.0f;
		float maxAngularVelocity = 47.124f;
		bool continuousCollision = false;
		bool allowSleeping = true;
		glm::bvec3 lockPosition{false};
		glm::bvec3 lockRotation{false};
		bool startActive = true;
		// Applied once by FlushPendingBodies when the body is created. Also the fallback
		// a velocity-setter export seeds when a script sets it before the body exists yet
		// (see PhysicsExports.cpp) - "add a body, then set its velocity" then behaves the
		// same whether or not the body has been baked yet.
		glm::vec3 initialVelocity{0.f, 0.f, 0.f};
		glm::vec3 initialAngularVelocity{0.f, 0.f, 0.f};

		// Runtime-only, deliberately unreflected: true once a script export has warned
		// that a force/torque/impulse call landed before this body existed, so a script
		// that calls one every frame in OnUpdate before the body bakes warns exactly once
		// instead of every frame. A component that gets its body never reaches the warning
		// branch again; RemoveBody/OnRigidBodyDestroyed remove the whole component, so a
		// rebuilt body starts this fresh rather than carrying a stale suppression forward.
		bool deferredCallWarned = false;
	};

	// (a "stay" query). Never serialized - it is pure runtime state.
	struct CollisionEventsComponent
	{
		std::vector<Entity> collisionEnter;
		std::vector<Entity> collisionExit;
		std::vector<Entity> triggerEnter;
		std::vector<Entity> triggerExit;
		std::vector<Entity> overlapping;
	};

	enum class JointType : uint8_t
	{
		Fixed,
		Point,
		Hinge,
		Distance,
		Slider,

		// Ball-and-socket with a separate swing (cone) limit and twist limit - the shape a
		// shoulder or hip needs and a Hinge cannot give (a Hinge only ever has ONE rotation
		// axis; a ball joint needs two: how far it swings off its resting direction, and how
		// far it twists around that direction). Deliberately the only ball-joint type here -
		// Jolt also ships a plain ConeConstraint, but equal twist limits already collapse
		// this one to a simple cone, so a second type would be redundant glue over the same
		// underlying Jolt class.
		SwingTwist,
	};

	struct JointComponent
	{
		JointType type = JointType::Fixed;
		Entity target{};
		glm::vec3 anchor{0.0f, 0.0f, 0.0f};
		glm::vec3 axis{0.0f, 1.0f, 0.0f};
		float minLimit = 0.0f;
		float maxLimit = 0.0f;
		float distance = -1.0f;

		// Swing Twist only: half-angle (radians) of the cone the far body can swing its
		// twist axis through before the limit stops it, applied symmetrically on both the
		// normal and plane axes (a single round cone, not an oval one - simpler than most
		// anatomical joints truly are, but a reasonable v1 approximation). Ignored by every
		// other joint type.
		float swingLimit = 0.0f;
		bool collideConnected = false;

		std::uint32_t constraintId = 0;
	};

	// One script/tool-created constraint. Same shape as JointComponent's own fields
	// (they drive the identical Jolt constraint types through PhysicsSystem's shared
	// CreateJointConstraint), duplicated here rather than reused because the two
	// serve different owners with different cardinality: JointComponent is the
	// Inspector/ragdoll model (one authored joint per entity, replaced wholesale by
	// EmplaceOrReplace), while a welded contraption routinely needs MORE THAN ONE
	// constraint on the same prop (three props welded in a line gives the middle one
	// two). `handle` is assigned the moment script requests the constraint - before
	// FlushPendingJoints ever runs - so PhysicsSystem::CreateFixedConstraint/
	// CreateDistanceConstraint can hand it back immediately; `created` tracks whether
	// the real Jolt constraint has been built yet.
	struct JointEntry
	{
		JointType type = JointType::Fixed;
		Entity target{};
		glm::vec3 anchor{0.0f, 0.0f, 0.0f};
		glm::vec3 axis{0.0f, 1.0f, 0.0f};
		float minLimit = 0.0f;
		float maxLimit = 0.0f;
		float distance = -1.0f;
		float swingLimit = 0.0f;
		bool collideConnected = false;

		std::uint32_t handle = 0;
		bool created = false;
	};

	// Script/tool-gun-created constraints living on the entity that owns them
	// (mirrors JointComponent's "self is body A, target is body B" convention).
	// Deliberately unreflected/not Inspector-editable - authored joints go through
	// JointComponent; this is the runtime API surface (Physics.CreateWeld etc.),
	// serialized separately (see PhysicsSerde.cpp) so a saved, welded contraption is
	// still welded on reload.
	struct ScriptJointsComponent
	{
		std::vector<JointEntry> joints;
	};

	// every frame by SyncTransforms so app code never needs to bake it into the
	struct PhysicsStateComponent
	{
		glm::vec3 prevPosition{};
		glm::quat prevRotation{1.f, 0.f, 0.f, 0.f};
		glm::vec3 currPosition{};
		glm::quat currRotation{1.f, 0.f, 0.f, 0.f};
		glm::vec3 scale{1.f, 1.f, 1.f};
	};

	// A Jolt CharacterVirtual-backed 3D player/NPC controller: a swept capsule with
	// slope/step/ground handling, distinct from RigidBodyComponent because a virtual
	// character is not a Jolt Body (see PhysicsSystem::StepCharacters). One entity
	// never carries both - see the Rigid Body/Collider conflict check in
	// PhysicsSystem::FlushPendingCharacters and FlushPendingBodies.
	//
	// The character's world position tracks its FEET (ground contact point), not its
	// centre - the convention Jolt's own CharacterVirtual uses - unlike every other
	// physics component in this file, which is centred on the entity origin.
	//
	// Has a Jolt inner rigid body (CharacterVirtualSettings::mInnerBodyShape, see
	// FlushPendingCharacters) so the character participates in the world the way any other
	// body does: raycasts/sphere casts hit it, and a fast dynamic body's own Jolt-side
	// integration collides against it instead of passing through in one step - needed for
	// this game's physics gun (raycast pick-up of props and characters alike) and its bot
	// throwing props at players. Kinematic; one extra BodyID per character (out of the
	// physics system's configured body budget) and one extra broad-phase entry, both kept
	// in sync for free by Jolt itself (CharacterVirtual's own constructor/destructor create
	// and destroy it, and UpdateInnerBodyTransform - called automatically from
	// SetPosition/SetRotation and at the end of every Update()/ExtendedUpdate() - keeps its
	// transform current). The one place that is NOT automatic: StepCharacters' shape-rebuild
	// path must call SetInnerBodyShape by hand after SetShape, or the inner body keeps
	// colliding as the character's OLD radius/halfHeight. Its user data is the same entity
	// id as everything else in this file, so a raycast or collision event against it needs
	// no special-casing to report the right entity.
	//
	// It carries no velocity of its own (position-only, matching Jolt's own CharacterVirtual
	// sample) - a prop resting against a MOVING character is still pushed every step by the
	// character's own contact solving (see StepCharacters), not by riding the inner body like
	// a physical platform. Fine for a sandbox where the character shoves things by walking
	// into them; revisit if a design ever needs a prop to feel carried.
	struct CharacterControllerComponent
	{
		// ---- Authored tunables (reflected: inspector + MCP editable) ----

		// Capsule radius, metres. 0.3 m is a typical first-person shoulder width: narrow
		// enough to clear a single-wide doorway, wide enough that walls don't feel razor-thin.
		float radius = 0.3f;

		// Half the capsule's straight cylindrical section, metres (the rounded caps add
		// radius on top of this). With the default radius this gives a 1.8 m standing
		// character - eye height for an average adult.
		float halfHeight = 0.6f;

		// Steepest ground the character can walk up without sliding, in radians (persisted;
		// the inspector shows degrees). 45 degrees matches the walkable-slope convention most
		// first-person shooters use - anything steeper reads as a wall, not a ramp.
		float maxSlopeAngle = glm::radians(45.0f);

		// Tallest ledge the character climbs automatically instead of being blocked by,
		// metres. 0.3 m clears an ordinary stair rise (15-20 cm) with margin, while still
		// stopping the character at a 1 m crate - the gap between those two numbers is what
		// makes "stair" and "obstacle" mean different things to the controller.
		float stepHeight = 0.3f;

		// How far down the character probes to stay glued to the ground on the way down a
		// slope or a stair, metres. Matched to stepHeight by default, so descending is exactly
		// as forgiving as ascending; too small and fast downhill movement goes briefly
		// airborne on every step, too large and the character snaps through drops it should
		// fall from.
		float groundSnapDistance = 0.3f;

		// Character mass, kilograms. Only affects how hard the character presses down
		// through a dynamic surface it stands on top of - never its own movement. 80 kg is
		// an average adult.
		float mass = 80.0f;

		// Hardest the character can shove another dynamic body, in newtons. Strong enough to
		// nudge ordinary sandbox props like crates, capped so it can't casually shove
		// something the size of a parked car - Jolt's own built-in default is 100 N; this is
		// 5x that.
		float maxPushForce = 500.0f;

		// Multiplies scene gravity for this character alone. 1 falls at the same rate as
		// every rigid body; a sandbox often wants this a little higher for snappier, more
		// responsive jumps without touching gravity for the whole scene.
		float gravityScale = 1.0f;

		// ---- Multiplayer seam (engine-visible; deliberately NOT reflected/serialized) ----

		// True on the peer that owns this character and should step it (host, or the client
		// whose player this is). False turns StepCharacters into a passive shadow: the
		// character's Jolt-side position/rotation track whatever already wrote
		// TransformComponent (replication) instead of integrating input/gravity itself, so a
		// peer never runs two independent simulations of the same character - the divergence
		// bug that already exists for 3D rigid bodies (PhysicsSystem::SyncTransforms has no
		// ownership check and overwrites a network-written transform every frame).
		//
		// Ownership is an app/net-layer concept (NetworkIdentity, NetworkContext) that this
		// engine-layer component cannot see or depend on. Net code is expected to flip this
		// flag the same way NetworkContext::SyncSimulationAuthority forces a non-owned 2D
		// body kinematic today; nothing in this engine layer flips it yet, so single-player
		// and every host character behaves exactly as if the field did not exist.
		bool locallySimulated = true;

		// ---- Script input (written by CharacterExports, consumed by StepCharacters) ----
		glm::vec3 desiredVelocity{0.0f, 0.0f, 0.0f};
		bool velocityOverride = false;
		float pendingJumpSpeed = 0.0f;

		// ---- Runtime output (written by StepCharacters, read by script/inspector) ----
		glm::vec3 velocity{0.0f, 0.0f, 0.0f};
		bool isGrounded = false;
		glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
	};

	// Marks one dynamic RigidBodyComponent/ColliderComponent entity as a bone of a
	// physics-driven ragdoll (see RagdollBuilder.hpp) - the entity is otherwise a
	// completely ordinary body: the physics gun, collision events, and every other
	// system that only knows about RigidBodyComponent already work on it unmodified.
	// Deliberately never a scene-hierarchy child of another bone: PhysicsSystem::
	// SyncTransforms calls ecs::SetWorldTransform once per body, which cascades to a
	// child's TransformComponent - two independently-simulated bones would fight over
	// which write wins depending on ECS iteration order. Every bone is a scene root;
	// this component (plus RagdollComponent::bones on the root) is the only thing that
	// still ties them together.
	struct RagdollBoneComponent
	{
		Entity root{};
		std::string boneName;

		// The glTF skeleton node name this bone's capsule was measured from at spawn
		// (e.g. "mixamorig:LeftForeArm") - DIFFERENT from boneName (the abstract role,
		// "LeftForearm"), and the only thing a later skin-drive lookup can match
		// against an AnimationDatabase's own node names. Empty for a ragdoll spawned
		// before this field existed (an old save/replay) - BuildRagdollSkinOverrides
		// skips a bone whose name cannot be resolved rather than guessing.
		std::string skinNodeName;

		// Fixed offset from this bone's own physics-body frame (capsule centre,
		// +Y along the limb) to its skeleton node's bind-pose frame, captured once at
		// spawn: inverse(bodyBindWorld) * nodeBindWorld. A rigid body never deforms,
		// so this stays correct for the ragdoll's entire lifetime - each frame,
		// nodeWorld(t) = bodyWorld(t) * skinNodeOffset reconstructs exactly where the
		// skinned mesh's joint is now, with no per-frame bind-pose recomputation.
		// Identity for a bone with no matched skeleton node (should not happen; see
		// RagdollBuilder.cpp).
		glm::mat4 skinNodeOffset{1.0f};
	};

	// Marks a SkinnedMeshComponent entity as driven by a ragdoll's physics bones
	// instead of its own animator, once attached (see RagdollSkinDrive.hpp). Never
	// serialized - like RagdollComponent, this only ever exists at runtime, created
	// after a ragdoll has already been spawned. ragdollRoot names the ragdoll (its
	// RagdollComponent::bones is the set of driving bones); an invalid or no-longer-
	// ragdoll root just means nothing overrides this frame - the mesh renders its
	// last sampled animation pose, same as any other skinned mesh.
	struct RagdollSkinDriveComponent
	{
		Entity ragdollRoot{};
	};

	// Lives on a ragdoll's root bone (see RagdollBuilder::Spawn) purely for group
	// bookkeeping - "every entity this ragdoll owns", for a caller that wants to act on
	// the whole ragdoll at once. Never serialized: a ragdoll is spawned at runtime, not
	// scene-authored.
	struct RagdollComponent
	{
		std::vector<Entity> bones;
	};

} // namespace aether
