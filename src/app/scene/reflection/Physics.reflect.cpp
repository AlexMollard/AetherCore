// Physics component reflection declarations. Demonstrates enum fields: the
// name<->value tables are plain C++ statics (so the preprocessor never sees their
// commas), referenced by AE_FIELD_ENUM.

#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "physics/PhysicsComponents.hpp"

using namespace aether;

namespace
{
	const reflect::EnumTable& MotionTypeEnum()
	{
		static const reflect::EnumTable t{{{"static", static_cast<int>(PhysicsMotionType::Static)},
		        {"kinematic", static_cast<int>(PhysicsMotionType::Kinematic)},
		        {"dynamic", static_cast<int>(PhysicsMotionType::Dynamic)}}};
		return t;
	}
	const reflect::EnumTable& ShapeTypeEnum()
	{
		static const reflect::EnumTable t{{{"box", static_cast<int>(PhysicsShapeType::Box)},
		        {"sphere", static_cast<int>(PhysicsShapeType::Sphere)},
		        {"capsule", static_cast<int>(PhysicsShapeType::Capsule)},
		        {"cylinder", static_cast<int>(PhysicsShapeType::Cylinder)}}};
		return t;
	}
	const reflect::EnumTable& JointTypeEnum()
	{
		static const reflect::EnumTable t{{{"fixed", static_cast<int>(JointType::Fixed)},
		        {"point", static_cast<int>(JointType::Point)},
		        {"hinge", static_cast<int>(JointType::Hinge)},
		        {"distance", static_cast<int>(JointType::Distance)},
		        {"slider", static_cast<int>(JointType::Slider)}}};
		return t;
	}
} // namespace

AE_COMPONENT(RigidBodyComponent, "Rigid Body", "Physics", ICON_FA_WEIGHT_HANGING)
	AE_FIELD_ENUM("motion", motionType, MotionTypeEnum())
	AE_FIELD_N("mass", mass, Float)
	AE_FIELD_N("linear_damping", linearDamping, Float)
	AE_FIELD_N("angular_damping", angularDamping, Float)
	AE_FIELD_N("gravity_factor", gravityFactor, Float)
	AE_FIELD_N("max_linear_velocity", maxLinearVelocity, Float)
	AE_FIELD_N("max_angular_velocity", maxAngularVelocity, Float)
	AE_FIELD_N("continuous_collision", continuousCollision, Bool)
	AE_FIELD_N("allow_sleeping", allowSleeping, Bool)
	AE_FIELD_N("start_active", startActive, Bool)
AE_COMPONENT_END()

AE_COMPONENT(ColliderComponent, "Collider", "Physics", ICON_FA_WEIGHT_HANGING)
	AE_FIELD_ENUM("shape", shape, ShapeTypeEnum())
	AE_FIELD_N("half_extents", halfExtents, Vec3)
	AE_FIELD_N("radius", radius, Float)
	AE_FIELD_N("half_height", halfHeight, Float)
	AE_FIELD_N("center", center, Vec3)
	AE_FIELD_N("friction", friction, Float)
	AE_FIELD_N("restitution", restitution, Float)
	AE_FIELD_N("is_sensor", isSensor, Bool)
	AE_NOT_ADDABLE() // the catalog adds it via per-shape entries (Box/Sphere/... Collider)
AE_COMPONENT_END()

AE_COMPONENT(JointComponent, "Joint", "Physics", ICON_FA_LINK)
	AE_FIELD_ENUM("type", type, JointTypeEnum())
	AE_FIELD_N("target", target, EntityRef)
	AE_FIELD_N("anchor", anchor, Vec3)
	AE_FIELD_N("axis", axis, Vec3)
	AE_FIELD_N("min_limit", minLimit, Float)
	AE_FIELD_N("max_limit", maxLimit, Float)
	AE_FIELD_N("distance", distance, Float)
	AE_FIELD_N("collide_connected", collideConnected, Bool)
AE_COMPONENT_END()
