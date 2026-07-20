#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "particles/ParticleComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"

using namespace aether;

namespace
{
	const reflect::EnumTable& ParticleBlendEnum()
	{
		static const reflect::EnumTable table{{
		        {"alpha", static_cast<int>(SpriteBlendMode::Alpha)},
		        {"additive", static_cast<int>(SpriteBlendMode::Additive)},
		        {"multiply", static_cast<int>(SpriteBlendMode::Multiply)},
		        {"opaque", static_cast<int>(SpriteBlendMode::Opaque)},
		}};
		return table;
	}
} // namespace

AE_COMPONENT(TileMapComponent, "Tile Map", "Rendering", ICON_FA_IMAGE)
AE_FIELD_N("tilemap", tilemapPath, String)
AE_FIELD_N("tint", tint, Color4)
AE_FIELD_N("sorting_layer", sortingLayer, Int)
AE_FIELD_N("order_in_layer", orderInLayer, Int)
AE_FIELD_N("visible_layers", visibleLayerMask, UInt)
AE_FIELD_N("visible", visible, Bool)
b.RequiresFeature(SceneFeatureFlags::Tilemaps);
AE_COMPONENT_END()

AE_COMPONENT(DayNightComponent, "Day Night", "Rendering", ICON_FA_CLOUD_SUN)
AE_FIELD_N("animate", animate, Bool)
AE_FIELD_N("time_of_day", timeOfDayHours, Float)
AE_FIELD_N("time_speed", timeSpeedSecondsPerSecond, Float)
b.RequiresFeature(SceneFeatureFlags::Lighting3D);
AE_COMPONENT_END()

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

AE_COMPONENT(ParallaxComponent, "Parallax", "Rendering", ICON_FA_IMAGE)
AE_FIELD_N("factor", factor, Vec2)
AE_FIELD_N("scroll_speed", scrollSpeed, Vec2)
AE_COMPONENT_END()

AE_COMPONENT(ParticleEmitterComponent, "Particle Emitter", "Rendering", ICON_FA_WAND_MAGIC_SPARKLES)
AE_FIELD_N("texture", texturePath, String)
AE_FIELD_N("rate", rate, Float)
AE_FIELD_N("burst_count", burstCount, UInt)
AE_FIELD_N("emit_on_start", emitOnStart, Bool)
AE_FIELD_N("emitting", emitting, Bool)
AE_FIELD_N("auto_destroy", autoDestroyWhenDone, Bool)
AE_FIELD_N("max_particles", maxParticles, UInt)
AE_FIELD_N("lifetime_min", lifetimeMin, Float)
AE_FIELD_N("lifetime_max", lifetimeMax, Float)
AE_FIELD_N("speed_min", speedMin, Float)
AE_FIELD_N("speed_max", speedMax, Float)
AE_FIELD_N("direction_deg", directionDeg, Float)
AE_FIELD_N("spread_deg", spreadDeg, Float)
AE_FIELD_N("gravity", gravity, Vec2)
AE_FIELD_N("start_size", startSize, Float)
AE_FIELD_N("end_size", endSize, Float)
AE_FIELD_N("start_color", startColor, Color4)
AE_FIELD_N("end_color", endColor, Color4)
AE_FIELD_ENUM("blend_mode", blendMode, ParticleBlendEnum())
AE_FIELD_N("sorting_layer", sortingLayer, Int)
AE_FIELD_N("order_in_layer", orderInLayer, Int)
AE_FIELD_N("collide_world", collideWorld, Bool)
AE_FIELD_N("collide_particles", collideParticles, Bool)
AE_FIELD_N("bounce", bounce, Float)
AE_FIELD_N("collision_damping", collisionDamping, Float)
AE_FIELD_N("collision_radius", collisionRadius, Float)
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
