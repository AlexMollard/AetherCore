#pragma once

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

#include "scene/Entity.hpp"

namespace aether
{
	enum class Body2DType : std::uint8_t
	{
		Static,
		Kinematic,
		Dynamic,
	};

	// Packed b2BodyId (b2StoreBodyId). 0 = invalid: Box2D's null id packs to 0.
	struct Physics2DBodyHandle
	{
		std::uint64_t value = 0;

		[[nodiscard]] bool IsValid() const noexcept
		{
			return value != 0;
		}
	};

	struct RigidBody2DComponent
	{
		Physics2DBodyHandle body; // runtime only - never serialized

		Body2DType bodyType = Body2DType::Dynamic;
		float gravityScale = 1.0f;
		float linearDamping = 0.0f;
		float angularDamping = 0.05f;
		bool fixedRotation = false;
		bool continuousCollision = false; // Box2D "bullet"
		bool allowSleeping = true;
		bool startAwake = true;
		glm::vec2 initialVelocity{0.0f};
		float initialAngularVelocity = 0.0f; // radians/s, +CCW
	};

	enum class Collider2DShape : std::uint8_t
	{
		Box,
		Circle,
		Capsule,
		Polygon,
	};

	struct Collider2DComponent
	{
		Collider2DShape shape = Collider2DShape::Box;
		glm::vec2 size{1.0f, 1.0f}; // box full extents, world units, before entity scale
		float radius = 0.5f;        // circle/capsule
		float capsuleHeight = 1.0f; // capsule end-to-end along local Y, >= 2*radius
		glm::vec2 offset{0.0f};
		float density = 1.0f;
		float friction = 0.5f;
		float restitution = 0.0f;
		bool isTrigger = false;
		// 32 filter bits authored; widened to Box2D's 64-bit filter at shape build.
		std::uint32_t categoryBits = 1;
		std::uint32_t maskBits = 0xFFFFFFFFu;
		std::int32_t groupIndex = 0;
		std::vector<glm::vec2> points; // convex polygon verts (Box2D caps hulls at 8)

		std::vector<std::uint64_t> shapes; // runtime packed b2ShapeIds - never serialized
	};

	// Populated by Physics2DSystem::DrainEvents each fixed step; enter/exit lists
	// are cleared at the start of each game-thread Update while 'overlapping' is a
	// live "stay" set. Never serialized - pure runtime state.
	struct CollisionEvents2DComponent
	{
		std::vector<Entity> collisionEnter;
		std::vector<Entity> collisionExit;
		std::vector<Entity> triggerEnter;
		std::vector<Entity> triggerExit;
		std::vector<Entity> overlapping;
	};

	enum class Joint2DType : std::uint8_t
	{
		Distance,
		Revolute,
		Prismatic,
		Weld,
	};

	// Joins this entity's body (Box2D body B) to the target's body (body A), so
	// motors/limits drive this entity relative to its target, Unity-style.
	struct Joint2DComponent
	{
		Joint2DType type = Joint2DType::Revolute;
		Entity target{};
		glm::vec2 anchor{0.0f};          // local to this body
		glm::vec2 connectedAnchor{0.0f}; // local to the target body
		glm::vec2 axis{1.0f, 0.0f};      // prismatic axis, in the target body's local space
		float minLimit = 0.0f; // revolute: radians; prismatic: units
		float maxLimit = 0.0f;
		float length = 1.0f; // distance joint rest length
		float motorSpeed = 0.0f;
		float maxMotorForce = 0.0f;
		bool enableLimit = false;
		bool enableMotor = false;
		bool collideConnected = false;

		std::uint64_t jointId = 0; // packed b2JointId, runtime only
	};

	// Fixed-step interpolation state, mirrors PhysicsStateComponent (3D).
	struct Physics2DStateComponent
	{
		glm::vec2 prevPosition{0.0f};
		float prevAngle = 0.0f;
		glm::vec2 currPosition{0.0f};
		float currAngle = 0.0f;
		float depthZ = 0.0f;   // transform Z preserved through sync
		glm::vec3 scale{1.0f}; // captured at body build; applied on sync
	};
} // namespace aether
