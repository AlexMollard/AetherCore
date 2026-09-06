// Custom scene serde for 3D physics and joints. Collider + RigidBody merge into one
// on-disk PhysicsRecord (with a derived collision layer), and joints reference another
// entity - captured as a scene-local index, resolved on apply through the created
// vector. Both need the serde context and the per-entity 2D/3D domain tie-break, so
// they cannot be plain reflected fields.

#include "scene/SceneComponentSerde.hpp"

#include "physics/PhysicsComponents.hpp"
#include "physics/PhysicsSystem.hpp"
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
			pr.meshSource = col->meshSource;
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
				pr.startActive = rb->startActive;
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
			                .meshSource = phys.meshSource,
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
			                .startActive = phys.startActive,
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

		void CaptureScriptJoints(SceneCaptureContext& c)
		{
			const auto* comp = c.world.TryGet<ScriptJointsComponent>(c.entity);
			if (comp == nullptr || comp->joints.empty())
			{
				return;
			}
			c.rec.scriptJoints.reserve(comp->joints.size());
			for (const JointEntry& entry: comp->joints)
			{
				ScriptJointRecord sjr;
				sjr.type = entry.type;
				if (entry.target.IsValid())
				{
					const auto it = c.indexOf.find(entry.target.id);
					sjr.targetIndex = it != c.indexOf.end() ? it->second : -1;
				}
				sjr.anchor = entry.anchor;
				sjr.axis = entry.axis;
				sjr.minLimit = entry.minLimit;
				sjr.maxLimit = entry.maxLimit;
				sjr.distance = entry.distance;
				sjr.swingLimit = entry.swingLimit;
				sjr.collideConnected = entry.collideConnected;
				c.rec.scriptJoints.push_back(sjr);
			}
		}

		// Restores each entry through PhysicsSystem::AddScriptJoint - the SAME path
		// CreateFixedConstraint/CreateDistanceConstraint use - so a reloaded
		// contraption's welds/ropes get freshly minted handles and owner bookkeeping
		// exactly as if script had just created them, rather than resurrecting the
		// pre-save handles (meaningless after a reload - nothing on the C# side holds
		// them across one). No PhysicsSystem registered (headless/no-scripting World,
		// same graceful-absence idiom as ScriptSerde.cpp) means no constraints - not
		// an error, there is nothing else that could apply them.
		void ApplyScriptJoints(SceneApplyContext& c)
		{
			if (c.rec.scriptJoints.empty() || !c.apply3DPhysics)
			{
				return;
			}
			auto* physics = static_cast<PhysicsSystem*>(c.world.FindSystem("PhysicsSystem"));
			if (physics == nullptr)
			{
				return;
			}
			for (const ScriptJointRecord& sjr: c.rec.scriptJoints)
			{
				Entity targetEntity{};
				if (sjr.targetIndex >= 0 && sjr.targetIndex < static_cast<int>(c.created.size()))
				{
					targetEntity = c.created[static_cast<std::size_t>(sjr.targetIndex)];
				}
				JointEntry entry;
				entry.type = sjr.type;
				entry.target = targetEntity;
				entry.anchor = sjr.anchor;
				entry.axis = sjr.axis;
				entry.minLimit = sjr.minLimit;
				entry.maxLimit = sjr.maxLimit;
				entry.distance = sjr.distance;
				entry.swingLimit = sjr.swingLimit;
				entry.collideConnected = sjr.collideConnected;
				physics->AddScriptJoint(c.world, c.entity, entry);
			}
		}

		AE_SCENE_SERDE(ScriptJoints, "Script Joints", 36, CaptureScriptJoints, ApplyScriptJoints)

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
