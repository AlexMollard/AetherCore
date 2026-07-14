#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"

using namespace aether;

AE_COMPONENT(NameComponent, "Name", "Core", ICON_FA_PEN)
AE_FIELD_N("name", name, String)
AE_COMPONENT_END()

AE_COMPONENT(OrbitCameraComponent, "Orbit Camera", "Rendering", ICON_FA_VIDEO)
AE_FIELD_N("target", target, Vec3)
AE_FIELD_N("yaw", yaw, Float)
AE_FIELD_N("pitch", pitch, Float)
AE_FIELD_N("distance", distance, Float)
AE_NOT_ADDABLE()
AE_COMPONENT_END()

AE_COMPONENT(LookAtComponent, "Look At", "Behaviors", ICON_FA_EYE)
AE_FIELD_N("target", target, Vec3)
AE_FIELD_N("keep_upright", keepUpright, Bool)
AE_COMPONENT_END()

AE_COMPONENT(RootMotionComponent, "Root Motion", "Behaviors", ICON_FA_PERSON_RUNNING)
AE_FIELD_N("apply_to_physics", applyToPhysics, Bool)
AE_FIELD_N("apply_to_transform", applyToTransform, Bool)
AE_FIELD_N("enabled", enabled, Bool)
AE_NOT_ADDABLE()
AE_COMPONENT_END()

AE_COMPONENT(AnimationBlendComponent, "Animation Blend", "Rendering", ICON_FA_FILM)
AE_FIELD_N("primary_clip", primaryClip, UInt)
AE_FIELD_N("secondary_clip", secondaryClip, UInt)
AE_FIELD_N("blend_weight", blendWeight, Float)
AE_FIELD_N("transition_speed", transitionSpeed, Float)
AE_NOT_ADDABLE()
AE_COMPONENT_END()
