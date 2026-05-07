#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

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

	// ── Shape descriptor components ───────────────────────────────────────────
	//
	// Emplace one of these on an entity (alongside a TransformComponent) to
	// request a physics body. PhysicsSystem::Update consumes the descriptor the
	// first time it sees it, creates the Jolt body, attaches RigidBodyComponent
	// + PhysicsStateComponent, sets the correct scaled transform, then removes
	// the descriptor. No explicit Add*Body calls needed in app code.

	struct BoxBodyDesc
	{
		glm::vec3 halfExtents{ 0.5f, 0.5f, 0.5f };
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
		PhysicsLayer layer = PhysicsLayer::Moving;
		float friction = 0.5f;
		float restitution = 0.0f;
		bool startActive = true;
		glm::vec3 initialVelocity{ 0.f, 0.f, 0.f };
	};

	struct SphereBodyDesc
	{
		float radius = 0.5f;
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
		PhysicsLayer layer = PhysicsLayer::Moving;
		float friction = 0.5f;
		float restitution = 0.0f;
		bool startActive = true;
		glm::vec3 initialVelocity{ 0.f, 0.f, 0.f };
	};

	struct CapsuleBodyDesc
	{
		float halfHeight = 0.5f;
		float radius = 0.25f;
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
		PhysicsLayer layer = PhysicsLayer::Moving;
		float friction = 0.5f;
		float restitution = 0.0f;
		bool startActive = true;
		glm::vec3 initialVelocity{ 0.f, 0.f, 0.f };
	};

	// ── Runtime components (managed by PhysicsSystem) ─────────────────────────

	// Attached to any entity that participates in physics simulation.
	// bodyId is a stable 32-bit handle - safe to use as a network replication key.
	struct RigidBodyComponent
	{
		JPH::BodyID bodyId{};
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
	};

	// Previous + current world-space state for sub-step interpolation.
	// Keeping render position decoupled from physics position lets the renderer
	// run at any framerate without stutter, and provides a natural place to apply
	// server-corrected positions during network reconciliation.
	//
	// scale is derived from the shape descriptor at creation time and reapplied
	// every frame by SyncTransforms so app code never needs to bake it into the
	// initial transform.
	struct PhysicsStateComponent
	{
		glm::vec3 prevPosition{};
		glm::quat prevRotation{ 1.f, 0.f, 0.f, 0.f };
		glm::vec3 currPosition{};
		glm::quat currRotation{ 1.f, 0.f, 0.f, 0.f };
		glm::vec3 scale{ 1.f, 1.f, 1.f };
	};

} // namespace aether
