// Physics2D component registration. Runtime fields (body/shape/joint handles,
// interpolation state) are omitted by not listing them; Collider2D polygon
// points get a bespoke drawer + serializer path (spatial data, not a neutral
// field), so they are not reflected here either.

#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "physics2d/Physics2DComponents.hpp"
#include "physics2d/Physics2DSystem.hpp"
#include "scene/World.hpp"

using namespace aether;

namespace
{
	// Inspector/MCP field edits recreate the backing Box2D objects so authored
	// state is always what simulates.
	void RebuildBody2D(World& world, Entity entity)
	{
		if (auto* physics = static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem")))
		{
			physics->RebuildBody(world, entity);
		}
	}

	void RebuildJoint2D(World& world, Entity entity)
	{
		if (auto* physics = static_cast<Physics2DSystem*>(world.FindSystem("Physics2DSystem")))
		{
			physics->RebuildJoint(world, entity);
		}
	}
	const reflect::EnumTable& Body2DTypeEnum()
	{
		static const reflect::EnumTable t{{{"static", static_cast<int>(Body2DType::Static)}, {"kinematic", static_cast<int>(Body2DType::Kinematic)}, {"dynamic", static_cast<int>(Body2DType::Dynamic)}}};
		return t;
	}

	const reflect::EnumTable& Collider2DShapeEnum()
	{
		static const reflect::EnumTable t{{{"box", static_cast<int>(Collider2DShape::Box)}, {"circle", static_cast<int>(Collider2DShape::Circle)}, {"capsule", static_cast<int>(Collider2DShape::Capsule)}, {"polygon", static_cast<int>(Collider2DShape::Polygon)}}};
		return t;
	}

	const reflect::EnumTable& Joint2DTypeEnum()
	{
		static const reflect::EnumTable t{{{"distance", static_cast<int>(Joint2DType::Distance)}, {"revolute", static_cast<int>(Joint2DType::Revolute)}, {"prismatic", static_cast<int>(Joint2DType::Prismatic)}, {"weld", static_cast<int>(Joint2DType::Weld)}}};
		return t;
	}
} // namespace

AE_COMPONENT(RigidBody2DComponent, "Rigid Body 2D", "Physics 2D", ICON_FA_WEIGHT_HANGING)
AE_FIELD_ENUM("body_type", bodyType, Body2DTypeEnum())
AE_FIELD_N("gravity_scale", gravityScale, Float)
AE_FIELD_N("linear_damping", linearDamping, Float)
AE_FIELD_N("angular_damping", angularDamping, Float)
AE_FIELD_N("fixed_rotation", fixedRotation, Bool)
AE_FIELD_N("continuous_collision", continuousCollision, Bool)
AE_FIELD_N("allow_sleeping", allowSleeping, Bool)
AE_FIELD_N("start_awake", startAwake, Bool)
b.PostSet(&RebuildBody2D);
b.RequiresFeature(SceneFeatureFlags::Physics2D);
b.ConflictsWith({"Rigid Body", "Box Collider", "Joint"});
AE_COMPONENT_END()

AE_COMPONENT(Collider2DComponent, "Collider 2D", "Physics 2D", ICON_FA_BOX_OPEN)
AE_FIELD_ENUM("shape", shape, Collider2DShapeEnum())
AE_FIELD_N("size", size, Vec2)
AE_FIELD_N("radius", radius, Float)
AE_FIELD_N("capsule_height", capsuleHeight, Float)
AE_FIELD_N("offset", offset, Vec2)
AE_FIELD_N("density", density, Float)
AE_FIELD_N("friction", friction, Float)
AE_FIELD_N("restitution", restitution, Float)
AE_FIELD_N("is_trigger", isTrigger, Bool)
AE_FIELD_N("category_bits", categoryBits, UInt)
AE_FIELD_N("mask_bits", maskBits, UInt)
AE_FIELD_N("group_index", groupIndex, Int)
b.PostSet(&RebuildBody2D);
b.RequiresFeature(SceneFeatureFlags::Physics2D);
b.ConflictsWith({"Rigid Body", "Box Collider", "Joint"});
AE_COMPONENT_END()

AE_COMPONENT(Joint2DComponent, "Joint 2D", "Physics 2D", ICON_FA_LINK)
AE_FIELD_ENUM("type", type, Joint2DTypeEnum())
AE_FIELD_N("target", target, EntityRef)
AE_FIELD_N("anchor", anchor, Vec2)
AE_FIELD_N("connected_anchor", connectedAnchor, Vec2)
AE_FIELD_N("axis", axis, Vec2)
AE_FIELD_N("min_limit", minLimit, Float)
AE_FIELD_N("max_limit", maxLimit, Float)
AE_FIELD_N("length", length, Float)
AE_FIELD_N("motor_speed", motorSpeed, Float)
AE_FIELD_N("max_motor_force", maxMotorForce, Float)
AE_FIELD_N("enable_limit", enableLimit, Bool)
AE_FIELD_N("enable_motor", enableMotor, Bool)
AE_FIELD_N("collide_connected", collideConnected, Bool)
b.PostSet(&RebuildJoint2D);
b.RequiresFeature(SceneFeatureFlags::Physics2D);
b.ConflictsWith({"Rigid Body", "Box Collider", "Joint"});
AE_COMPONENT_END()
