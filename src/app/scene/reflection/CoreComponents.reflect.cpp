// Core component reflection declarations (lights, cameras, skinned, transform,
//
// One AE_COMPONENT block per component replaces its hand-written entries in the
// MCP field registry, ComponentCatalog, (later) SceneSerializer and inspector.
// Field names match the existing scene-TOML keys so the serializer migration
// preserves the on-disk format. Runtime/cached fields are omitted by not listing
// them. Follow-up batches add the remaining components (physics, effects,
// material, UI, custom-widget components) as their own *.reflect.cpp files.

#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/TransformUtils.hpp"

using namespace aether;
using aether::reflect::FieldType;
using aether::reflect::FieldValue;

// ── Lights ──────────────────────────────────────────────────────────────────
AE_COMPONENT(PointLightComponent, "Point Light", "Rendering", ICON_FA_LIGHTBULB)
AE_FIELD_N("color", color, Color3)
AE_FIELD_R(intensity, Float, 0.0f, 1000.0f)
AE_FIELD_R(radius, Float, 0.0f, 500.0f)
AE_FIELD_N("shadow", castsShadow, Bool)
AE_COMPONENT_END()

AE_COMPONENT(SpotLightComponent, "Spot Light", "Rendering", ICON_FA_LIGHTBULB)
AE_FIELD_N("color", color, Color3)
AE_FIELD_R(intensity, Float, 0.0f, 1000.0f)
AE_FIELD_R(radius, Float, 0.0f, 500.0f)
AE_FIELD_ANGLE_AS("inner_angle_deg", innerAngleRad, "inner_rad")
AE_FIELD_ANGLE_AS("outer_angle_deg", outerAngleRad, "outer_rad")
AE_FIELD_N("shadow", castsShadow, Bool)
AE_COMPONENT_END()

// ── Rendering / animation ───────────────────────────────────────────────────
AE_COMPONENT(CameraComponent, "Camera", "Rendering", ICON_FA_VIDEO)
AE_FIELD_N("fov", fovDegrees, Float)
AE_FIELD_N("near", nearPlane, Float)
AE_FIELD_N("far", farPlane, Float)
AE_COMPONENT_END()

AE_COMPONENT(SkinnedMeshComponent, "Skinned Mesh", "Rendering", ICON_FA_FILM)
AE_FIELD_N("clip", clipIndex, UInt)
AE_FIELD_N("time", animTime, Float)
AE_FIELD_N("speed", playbackSpeed, Float)
AE_FIELD_N("looping", looping, Bool)
AE_NOT_ADDABLE() // comes with a skinned model (needs an AnimationDatabase)
AE_COMPONENT_END()

// Transform is stored as a matrix; expose the authored TRS as computed fields.
AE_COMPONENT(TransformComponent, "Transform", "Core", ICON_FA_UP_DOWN_LEFT_RIGHT)
// Lambdas wrapped in () so their internal commas aren't read as macro args.
AE_FIELD_CUSTOM(
        "position",
        Vec3,
        [](const void* c)
        {
	        glm::vec3 p;
	        glm::vec3 e;
	        glm::vec3 s;
	        DecomposeTRS(static_cast<const TransformComponent*>(c)->localToWorld, p, e, s);
	        return reflect::MakeValue(p);
        },
        [](void* c, const FieldValue& v)
        {
	        auto* t = static_cast<TransformComponent*>(c);
	        glm::vec3 p;
	        glm::vec3 e;
	        glm::vec3 s;
	        DecomposeTRS(t->localToWorld, p, e, s);
	        t->localToWorld = ComposeTransform(glm::vec3(v.vec), e, s);
        })
AE_FIELD_CUSTOM(
        "euler",
        Vec3,
        [](const void* c)
        {
	        glm::vec3 p;
	        glm::vec3 e;
	        glm::vec3 s;
	        DecomposeTRS(static_cast<const TransformComponent*>(c)->localToWorld, p, e, s);
	        return reflect::MakeValue(e);
        },
        [](void* c, const FieldValue& v)
        {
	        auto* t = static_cast<TransformComponent*>(c);
	        glm::vec3 p;
	        glm::vec3 e;
	        glm::vec3 s;
	        DecomposeTRS(t->localToWorld, p, e, s);
	        t->localToWorld = ComposeTransform(p, glm::vec3(v.vec), s);
        })
AE_FIELD_CUSTOM(
        "scale",
        Vec3,
        [](const void* c)
        {
	        glm::vec3 p;
	        glm::vec3 e;
	        glm::vec3 s;
	        DecomposeTRS(static_cast<const TransformComponent*>(c)->localToWorld, p, e, s);
	        return reflect::MakeValue(s);
        },
        [](void* c, const FieldValue& v)
        {
	        auto* t = static_cast<TransformComponent*>(c);
	        glm::vec3 p;
	        glm::vec3 e;
	        glm::vec3 s;
	        DecomposeTRS(t->localToWorld, p, e, s);
	        t->localToWorld = ComposeTransform(p, e, glm::vec3(v.vec));
        })
AE_COMPONENT_END()

// ── Behaviors (procedural motion) ───────────────────────────────────────────
AE_COMPONENT(SpinComponent, "Spin", "Behaviors", ICON_FA_ROTATE)
AE_FIELD_N("euler_deg_per_sec", eulerDegPerSec, Vec3)
AE_COMPONENT_END()

AE_COMPONENT(BobComponent, "Bob", "Behaviors", ICON_FA_WAVE_SQUARE)
AE_FIELD_N("amplitude", amplitude, Float)
AE_FIELD_N("frequency", frequency, Float)
AE_FIELD_N("phase", phase, Float)
AE_COMPONENT_END()

AE_COMPONENT(OrbitComponent, "Orbit", "Behaviors", ICON_FA_CIRCLE_NOTCH)
AE_FIELD_N("center", center, Vec3)
AE_FIELD_N("radius", radius, Float)
AE_FIELD_N("speed_deg", angularSpeedDeg, Float)
AE_FIELD_N("angle_deg", angleDeg, Float)
AE_FIELD_N("yaw_offset_deg", yawOffsetDeg, Float)
AE_FIELD_N("height", height, Float)
AE_COMPONENT_END()

AE_COMPONENT(ScalePulseComponent, "Scale Pulse", "Behaviors", ICON_FA_EXPAND)
AE_FIELD_N("amplitude", amplitude, Float)
AE_FIELD_N("frequency", frequency, Float)
AE_FIELD_N("phase", phase, Float)
AE_COMPONENT_END()

AE_COMPONENT(MaterialPulseComponent, "Material Pulse", "Behaviors", ICON_FA_HEART_PULSE)
AE_FIELD_N("emissive_a", emissiveA, Color3)
AE_FIELD_N("emissive_b", emissiveB, Color3)
AE_FIELD_N("frequency", frequency, Float)
AE_COMPONENT_END()
