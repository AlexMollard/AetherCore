#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

namespace aether
{
	// Collision object layer — controls what a body collides with.
	// Designed with networking in mind: static geometry never needs net sync,
	// moving bodies need regular position/velocity replication.
	enum class PhysicsLayer : uint8_t
	{
		NonMoving = 0, // Static world geometry
		Moving    = 1, // Dynamic and kinematic bodies (players, objects)
		Sensor    = 2, // Trigger volumes (no collision response)
	};

	// How the physics engine drives a body's position.
	enum class PhysicsMotionType : uint8_t
	{
		Static,    // Immovable; never added to the network sync list
		Kinematic, // Moved by game code; velocity used for contact solving
		Dynamic,   // Fully simulated by the physics engine
	};

	// Attached to any entity that participates in physics simulation.
	// bodyId is a stable 32-bit handle — safe to use as a network replication key.
	struct RigidBodyComponent
	{
		JPH::BodyID       bodyId{};
		PhysicsMotionType motionType = PhysicsMotionType::Dynamic;
	};

	// Previous + current world-space state for sub-step interpolation.
	// Keeping render position decoupled from physics position lets the renderer
	// run at any framerate without stutter, and provides a natural place to apply
	// server-corrected positions during network reconciliation.
	struct PhysicsStateComponent
	{
		glm::vec3 prevPosition{};
		glm::quat prevRotation{ 1.f, 0.f, 0.f, 0.f };
		glm::vec3 currPosition{};
		glm::quat currRotation{ 1.f, 0.f, 0.f, 0.f };
	};

} // namespace aether
