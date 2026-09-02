// name<->value tables are plain C++ statics (so the preprocessor never sees their

#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "physics/PhysicsComponents.hpp"

using namespace aether;

namespace
{
	const reflect::EnumTable& MotionTypeEnum()
	{
		static const reflect::EnumTable t{{{"static", static_cast<int>(PhysicsMotionType::Static)}, {"kinematic", static_cast<int>(PhysicsMotionType::Kinematic)}, {"dynamic", static_cast<int>(PhysicsMotionType::Dynamic)}}};
		return t;
	}

	const reflect::EnumTable& ShapeTypeEnum()
	{
		static const reflect::EnumTable t{
		        {{"box", static_cast<int>(PhysicsShapeType::Box)}, {"sphere", static_cast<int>(PhysicsShapeType::Sphere)}, {"capsule", static_cast<int>(PhysicsShapeType::Capsule)}, {"cylinder", static_cast<int>(PhysicsShapeType::Cylinder)}}};
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
AE_FIELD_NT("mass", mass, Float, "Kilograms. Leave it at 0 to let the shape and its density work it out - an explicit mass only overrides that.")
AE_FIELD_NT("linear_damping", linearDamping, Float, "How quickly straight-line motion bleeds off on its own, independent of friction or collisions. 0 coasts forever.")
AE_FIELD_NT("angular_damping", angularDamping, Float, "The same bleed-off for spin. 0 keeps rotating forever.")
AE_FIELD_NT("gravity_factor", gravityFactor, Float, "Multiplies the scene gravity for this body alone: 1 is normal, 0 floats, 0.5 is moon-like, negative falls upward.")
AE_FIELD_NT("max_linear_velocity", maxLinearVelocity, Float, "Hard speed clamp in units per second. A body that hits this stops accelerating, however hard it is pushed.")
AE_FIELD_NT("max_angular_velocity", maxAngularVelocity, Float, "The same clamp for spin, in radians per second.")
AE_FIELD_NT("continuous_collision", continuousCollision, Bool, "Sweep this body between frames instead of testing where it lands, so something fast cannot pass through a thin wall. Costs more, so it is worth it for bullets and not for crates.")
AE_FIELD_NT("allow_sleeping", allowSleeping, Bool, "Let the body stop simulating once it settles. Worth knowing: a sleeping body ignores velocity you set from script until something wakes it, which reads exactly like the write being lost.")
AE_FIELD_NT("start_active", startActive, Bool, "Whether the body begins awake. Off means it waits to be touched, pushed or woken before it simulates at all.")
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(ColliderComponent, "Collider", "Physics", ICON_FA_DRAW_POLYGON)
AE_FIELD_ENUM("shape", shape, ShapeTypeEnum())
AE_FIELD_NT("half_extents", halfExtents, Vec3, "Half the box's size on each axis, so 0.5 is a one-unit cube. Box shapes only.")
AE_FIELD_NT("radius", radius, Float, "Sphere, capsule and cylinder radius. Ignored by a box, which uses half_extents.")
AE_FIELD_NT("half_height", halfHeight, Float, "Half the straight section of a capsule or cylinder, measured from the centre. A capsule's caps add radius on top of it.")
AE_FIELD_NT("center", center, Vec3, "Offsets the shape from the entity's origin without moving the entity - how you sit a collider around a mesh whose pivot is at its feet.")
AE_FIELD_NT("friction", friction, Float, "How much sliding contact resists. 0 is ice, 1 is rubber; the pair in contact is combined, so one slippery surface is enough to slide.")
AE_FIELD_NT("restitution", restitution, Float, "Bounciness. 0 lands dead, 1 returns all the energy it arrived with.")
AE_FIELD_NT("is_sensor", isSensor, Bool, "Report overlaps without pushing anything: things pass straight through while collision events still fire. This is how a trigger volume is made.")
AE_NOT_ADDABLE()
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
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()
