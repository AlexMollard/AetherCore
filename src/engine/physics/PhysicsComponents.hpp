#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	// Collision object layer - controls what a body collides with.
	// Designed with networking in mind: static geometry never needs net sync,
	// moving bodies need regular position/velocity replication.
	enum class PhysicsLayer : uint8_t
	{
		NonMoving = 0, // Static world geometry
		Moving = 1,    // Dynamic and kinematic bodies (players, objects)
		Sensor = 2,    // Trigger volumes (no collision response)
	};

	// How the physics engine drives a body's position.
	enum class PhysicsMotionType : uint8_t
	{
		Static,    // Immovable; never added to the network sync list
		Kinematic, // Moved by game code; velocity used for contact solving
		Dynamic,   // Fully simulated by the physics engine
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

	// Collider primitive kinds. Cylinder joins the original three shapes.
	enum class PhysicsShapeType : uint8_t
	{
		Box,
		Sphere,
		Capsule,
		Cylinder,
	};

	// -- Authoring components (Unity-style RigidBody + Collider) ---------------
	//
	// A physics body is described by two persistent, editable components:
	//   - ColliderComponent: the shape, its dimensions, and its surface material
	//     (friction / restitution / trigger flag).
	//   - RigidBodyComponent: how the body moves (motion type) plus its body-level
	//     tunables (mass, damping, gravity factor, CCD, axis-locks).
	// PhysicsSystem bakes the Jolt body from these the first frame it sees a
	// ColliderComponent without a live body. A ColliderComponent with no
	// RigidBodyComponent bakes as a Static body (PhysicsSystem adds the
	// RigidBodyComponent as the body handle holder). Both components survive body
	// creation, so the inspector keeps editing them and the scene serializes them.

	// Shape + dimensions + surface material of a collider.
	struct ColliderComponent
	{
		PhysicsShapeType shape = PhysicsShapeType::Box;
		glm::vec3 halfExtents{0.5f, 0.5f, 0.5f}; // Box
		float radius = 0.5f;                     // Sphere / Capsule / Cylinder
		float halfHeight = 0.5f;                 // Capsule / Cylinder (half of the straight section)
		glm::vec3 center{0.0f, 0.0f, 0.0f};      // local offset of the shape from the entity origin

		float friction = 0.5f;
		float restitution = 0.0f;    // bounciness (0 = none, 1 = perfectly elastic)
		bool isSensor = false;       // trigger: reports overlaps, no collision response
		PhysicsLayer layer = PhysicsLayer::Moving; // broadphase layer (sensor forces Sensor)
	};

	// Runtime body handle + how the body moves + body-level tunables. Present on
	// every physics entity (auto-added for collider-only static bodies). body is
	// an engine-level stable 32-bit handle - safe as a network replication key,
	// and invalid until PhysicsSystem bakes the body.
	struct RigidBodyComponent
	{
		PhysicsBodyHandle body;
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
		float mass = 0.0f; // 0 = auto (computed from the collider shape)
		float linearDamping = 0.05f;
		float angularDamping = 0.05f;
		float gravityFactor = 1.0f;
		float maxLinearVelocity = 500.0f;   // Jolt default clamp (m/s)
		float maxAngularVelocity = 47.124f; // Jolt default clamp (rad/s ~ 0.25*pi*60)
		bool continuousCollision = false;   // CCD for fast movers
		bool allowSleeping = true;          // let the solver deactivate a resting body
		glm::bvec3 lockPosition{false};     // freeze translation per world axis
		glm::bvec3 lockRotation{false};     // freeze rotation per world axis
		bool startActive = true;
		glm::vec3 initialVelocity{0.f, 0.f, 0.f};
	};

	// Opt-in per-entity collision/trigger events. Add this component to an entity
	// to receive them; PhysicsSystem refreshes the lists after each physics step
	// (and each editor tick). enter/exit lists hold the OTHER entity and are
	// cleared every frame; `overlapping` is the persistent current-contact set
	// (a "stay" query). Never serialized - it is pure runtime state.
	struct CollisionEventsComponent
	{
		std::vector<Entity> collisionEnter; // solid contacts that began this frame
		std::vector<Entity> collisionExit;  // solid contacts that ended this frame
		std::vector<Entity> triggerEnter;   // sensor overlaps that began this frame
		std::vector<Entity> triggerExit;    // sensor overlaps that ended this frame
		std::vector<Entity> overlapping;    // everything currently in contact
	};

	// -- Joints / constraints --------------------------------------------------

	enum class JointType : uint8_t
	{
		Fixed,    // rigidly weld two bodies
		Point,    // ball-socket: bodies share an anchor point, rotate freely
		Hinge,    // rotate about a single axis (optionally limited)
		Distance, // keep two anchor points a fixed/limited distance apart
		Slider,   // translate along a single axis (optionally limited)
	};

	// Constrains this entity's body to a target body (or the world when target is
	// invalid). Managed by PhysicsSystem: the constraint is created once both
	// bodies exist and destroyed when the joint or either body goes away.
	struct JointComponent
	{
		JointType type = JointType::Fixed;
		Entity target{};                     // other body; invalid = attach to the world
		glm::vec3 anchor{0.0f, 0.0f, 0.0f};  // world-space anchor at creation (Point/Hinge/Slider/Distance)
		glm::vec3 axis{0.0f, 1.0f, 0.0f};    // world-space axis (Hinge/Slider)
		float minLimit = 0.0f;               // Hinge angle (rad) / Slider distance min; min>=max = free
		float maxLimit = 0.0f;               // Hinge angle (rad) / Slider distance max
		float distance = -1.0f;              // Distance joint target (<0 = current distance at creation)
		bool collideConnected = false;       // let the two connected bodies still collide

		std::uint32_t constraintId = 0; // runtime: 0 = not created yet
	};

	// Previous + current world-space state for sub-step interpolation.
	// Keeping render position decoupled from physics position lets the renderer
	// run at any framerate without stutter, and provides a natural place to apply
	// server-corrected positions during network reconciliation.
	//
	// scale is derived from the collider dimensions at creation time and reapplied
	// every frame by SyncTransforms so app code never needs to bake it into the
	// initial transform.
	struct PhysicsStateComponent
	{
		glm::vec3 prevPosition{};
		glm::quat prevRotation{1.f, 0.f, 0.f, 0.f};
		glm::vec3 currPosition{};
		glm::quat currRotation{1.f, 0.f, 0.f, 0.f};
		glm::vec3 scale{1.f, 1.f, 1.f};
	};

} // namespace aether
