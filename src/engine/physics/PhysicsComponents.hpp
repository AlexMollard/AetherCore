#pragma once

#include <cstdint>
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
		glm::vec3 initialVelocity{0.f, 0.f, 0.f};
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
		bool collideConnected = false;

		std::uint32_t constraintId = 0;
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

} // namespace aether
