// Custom scene serde for 3D physics and joints. Collider + RigidBody merge into one
// on-disk PhysicsRecord (with a derived collision layer), and joints reference another
// entity - captured as a scene-local index, resolved on apply through the created
// vector. Both need the serde context and the per-entity 2D/3D domain tie-break, so
// they cannot be plain reflected fields.

#include "scene/SceneComponentSerde.hpp"

#include "physics/PhysicsComponents.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "scene/World.hpp"

namespace aether::app::scene
{
	namespace
	{
		void CapturePhysics3D(SceneCaptureContext& c)
		{
			const auto* col = c.world.TryGet<ColliderComponent>(c.entity);
			if (col == nullptr)
			{
				return;
			}
			PhysicsRecord pr{.shapeType = col->shape, .halfExtents = col->halfExtents, .radius = col->radius, .halfHeight = col->halfHeight};
			pr.center = col->center;
			pr.friction = col->friction;
			pr.restitution = col->restitution;
			pr.isSensor = col->isSensor;
			if (const auto* rb = c.world.TryGet<RigidBodyComponent>(c.entity))
			{
				pr.motionType = rb->motionType;
				pr.mass = rb->mass;
				pr.linearDamping = rb->linearDamping;
				pr.angularDamping = rb->angularDamping;
				pr.gravityFactor = rb->gravityFactor;
				pr.maxLinearVelocity = rb->maxLinearVelocity;
				pr.maxAngularVelocity = rb->maxAngularVelocity;
				pr.continuousCollision = rb->continuousCollision;
				pr.allowSleeping = rb->allowSleeping;
				pr.lockPosition = rb->lockPosition;
				pr.lockRotation = rb->lockRotation;
			}
			else
			{
				pr.motionType = PhysicsMotionType::Static;
			}
			c.rec.physics = pr;
		}

		void ApplyPhysics3D(SceneApplyContext& c)
		{
			if (!c.rec.physics || !c.apply3DPhysics)
			{
				return;
			}
			const PhysicsRecord& phys = *c.rec.physics;
			c.world.Emplace<ColliderComponent>(c.entity,
			        ColliderComponent{
			                .shape = phys.shapeType,
			                .halfExtents = phys.halfExtents,
			                .radius = phys.radius,
			                .halfHeight = phys.halfHeight,
			                .center = phys.center,
			                .friction = phys.friction,
			                .restitution = phys.restitution,
			                .isSensor = phys.isSensor,
			                .layer = phys.isSensor ? PhysicsLayer::Sensor : PhysicsLayer::Moving,
			        });
			c.world.Emplace<RigidBodyComponent>(c.entity,
			        RigidBodyComponent{
			                .motionType = phys.motionType,
			                .mass = phys.mass,
			                .linearDamping = phys.linearDamping,
			                .angularDamping = phys.angularDamping,
			                .gravityFactor = phys.gravityFactor,
			                .maxLinearVelocity = phys.maxLinearVelocity,
			                .maxAngularVelocity = phys.maxAngularVelocity,
			                .continuousCollision = phys.continuousCollision,
			                .allowSleeping = phys.allowSleeping,
			                .lockPosition = phys.lockPosition,
			                .lockRotation = phys.lockRotation,
			        });
		}

		AE_SCENE_SERDE(Physics3D, "Rigid Body", 30, CapturePhysics3D, ApplyPhysics3D)

		void CaptureJoint(SceneCaptureContext& c)
		{
			const auto* j = c.world.TryGet<JointComponent>(c.entity);
			if (j == nullptr)
			{
				return;
			}
			JointRecord jr;
			jr.type = j->type;
			if (j->target.IsValid())
			{
				const auto it = c.indexOf.find(j->target.id);
				jr.targetIndex = it != c.indexOf.end() ? it->second : -1;
			}
			jr.anchor = j->anchor;
			jr.axis = j->axis;
			jr.minLimit = j->minLimit;
			jr.maxLimit = j->maxLimit;
			jr.distance = j->distance;
			jr.collideConnected = j->collideConnected;
			c.rec.joint = jr;
		}

		void ApplyJoint(SceneApplyContext& c)
		{
			if (!c.rec.joint || !c.apply3DPhysics)
			{
				return;
			}
			const JointRecord& jr = *c.rec.joint;
			Entity targetEntity{};
			if (jr.targetIndex >= 0 && jr.targetIndex < static_cast<int>(c.created.size()))
			{
				targetEntity = c.created[static_cast<std::size_t>(jr.targetIndex)];
			}
			c.world.Emplace<JointComponent>(c.entity,
			        JointComponent{
			                .type = jr.type,
			                .target = targetEntity,
			                .anchor = jr.anchor,
			                .axis = jr.axis,
			                .minLimit = jr.minLimit,
			                .maxLimit = jr.maxLimit,
			                .distance = jr.distance,
			                .collideConnected = jr.collideConnected,
			        });
		}

		AE_SCENE_SERDE(Joint, "Joint", 35, CaptureJoint, ApplyJoint)

		void CaptureJoint2D(SceneCaptureContext& c)
		{
			const auto* j2d = c.world.TryGet<Joint2DComponent>(c.entity);
			if (j2d == nullptr)
			{
				return;
			}
			Joint2DComponent copy = *j2d;
			copy.jointId = 0;
			if (copy.target.IsValid())
			{
				const auto it = c.indexOf.find(copy.target.id);
				c.rec.joint2DTargetIndex = it != c.indexOf.end() ? it->second : -1;
			}
			copy.target = {};
			c.rec.joint2D = std::move(copy);
		}

		void ApplyJoint2D(SceneApplyContext& c)
		{
			if (!c.rec.joint2D || !c.apply2DPhysics)
			{
				return;
			}
			Joint2DComponent component = *c.rec.joint2D;
			if (c.rec.joint2DTargetIndex >= 0 && c.rec.joint2DTargetIndex < static_cast<int>(c.created.size()))
			{
				component.target = c.created[static_cast<std::size_t>(c.rec.joint2DTargetIndex)];
			}
			c.world.EmplaceOrReplace<Joint2DComponent>(c.entity, component);
		}

		AE_SCENE_SERDE(Joint2D, "Joint 2D", 35, CaptureJoint2D, ApplyJoint2D)
	} // namespace
} // namespace aether::app::scene
