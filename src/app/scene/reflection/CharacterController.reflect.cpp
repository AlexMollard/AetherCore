// A plain reflected, generically-serialized component (see AE_GENERIC_SERIALIZE) - the
// physics-thread-only runtime state (velocity/isGrounded/groundNormal/desiredVelocity/...)
// lives on CharacterControllerComponent too, but is deliberately left off this declaration:
// only authored tunables are reflected, so they are the only fields inspector/MCP/TOML
// ever see or persist. See PhysicsSystem::FlushPendingCharacters/StepCharacters.

#include "scene/reflection/Reflection.hpp"

#include "Icons.hpp"
#include "physics/PhysicsComponents.hpp"

using namespace aether;

AE_COMPONENT(CharacterControllerComponent, "Character Controller", "Physics", ICON_FA_PERSON_WALKING)
AE_FIELD_NT("radius", radius, Float, "Capsule radius in metres. 0.3 m is a typical first-person shoulder width - narrow enough to clear a single-wide doorway, wide enough that walls don't feel razor-thin.")
AE_FIELD_NT("half_height", halfHeight, Float, "Half the capsule's straight cylindrical section, in metres (the rounded caps add radius on top). With the default radius this gives a 1.8 m standing character - eye height for an average adult.")
AE_FIELD_ANGLE_AS("max_slope_angle", maxSlopeAngle, "max_slope_angle")
AE_FIELD_NT("step_height", stepHeight, Float, "Tallest ledge the character climbs automatically instead of being blocked by, in metres. 0.3 m clears an ordinary stair rise (15-20 cm) with margin, while still stopping the character at a 1 m crate.")
AE_FIELD_NT("ground_snap_distance", groundSnapDistance, Float, "How far down the character probes to stay glued to the ground going down a slope or a stair, in metres. Matched to step_height by default, so descending is exactly as forgiving as ascending.")
AE_FIELD_NT("mass", mass, Float, "Character mass in kilograms. Only affects how hard the character presses down through a dynamic surface it stands on - never its own movement. 80 kg is an average adult.")
AE_FIELD_NT("max_push_force", maxPushForce, Float, "Hardest the character can shove another dynamic body, in newtons. Strong enough to nudge ordinary sandbox props like crates, capped so it can't casually shove something the size of a parked car.")
AE_FIELD_NT("gravity_scale", gravityScale, Float, "Multiplies scene gravity for this character alone. 1 falls at the same rate as every rigid body; a sandbox often wants this a little higher for snappier jumps without touching gravity for the whole scene.")
b.RequiresFeature(SceneFeatureFlags::Physics3D);
b.ConflictsWith({"Rigid Body", "Collider"});
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
