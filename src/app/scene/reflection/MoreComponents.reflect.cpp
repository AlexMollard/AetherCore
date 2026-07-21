#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "particles/ParticleComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "ui/UiComponents.hpp"

using namespace aether;

// AE_COMPONENT pastes the type token, so it needs an unqualified name; alias the
// namespaced UI components (and reach their nested enums through the alias).
using UiCanvasComponent = aether::ui::UICanvas;
using UiRectComponent = aether::ui::UIRect;
using UiTextComponent = aether::ui::UIText;
using UiImageComponent = aether::ui::UIImage;
using UiSelectableComponent = aether::ui::UISelectable;

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

	const reflect::EnumTable& UiScaleModeEnum()
	{
		static const reflect::EnumTable table{{
		        {"constant_pixel", static_cast<int>(UiCanvasComponent::ScaleMode::ConstantPixel)},
		        {"scale_with_reference", static_cast<int>(UiCanvasComponent::ScaleMode::ScaleWithReference)},
		}};
		return table;
	}

	const reflect::EnumTable& UiHAlignEnum()
	{
		static const reflect::EnumTable table{{
		        {"left", static_cast<int>(UiTextComponent::HAlign::Left)},
		        {"center", static_cast<int>(UiTextComponent::HAlign::Center)},
		        {"right", static_cast<int>(UiTextComponent::HAlign::Right)},
		}};
		return table;
	}

	const reflect::EnumTable& UiVAlignEnum()
	{
		static const reflect::EnumTable table{{
		        {"top", static_cast<int>(UiTextComponent::VAlign::Top)},
		        {"middle", static_cast<int>(UiTextComponent::VAlign::Middle)},
		        {"bottom", static_cast<int>(UiTextComponent::VAlign::Bottom)},
		}};
		return table;
	}
} // namespace

AE_COMPONENT(TileMapComponent, "Tile Map", "Rendering", ICON_FA_TABLE_CELLS)
AE_FIELD_N("tilemap", tilemapPath, String)
AE_FIELD_N("tint", tint, Color4)
AE_FIELD_N("sorting_layer", sortingLayer, Int)
AE_FIELD_N("order_in_layer", orderInLayer, Int)
AE_FIELD_N("visible_layers", visibleLayerMask, UInt)
AE_FIELD_N("visible", visible, Bool)
b.RequiresFeature(SceneFeatureFlags::Tilemaps);
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(DayNightComponent, "Day Night", "Rendering", ICON_FA_CLOUD_SUN)
AE_FIELD_N("animate", animate, Bool)
AE_FIELD_N("time_of_day", timeOfDayHours, Float)
AE_FIELD_N("time_speed", timeSpeedSecondsPerSecond, Float)
b.RequiresFeature(SceneFeatureFlags::Lighting3D);
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(NameComponent, "Name", "Core", ICON_FA_TAG)
AE_FIELD_N("name", name, String)
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(OrbitCameraComponent, "Orbit Camera", "Rendering", ICON_FA_VIDEO)
AE_FIELD_N("target", target, Vec3)
AE_FIELD_N("yaw", yaw, Float)
AE_FIELD_N("pitch", pitch, Float)
AE_FIELD_N("distance", distance, Float)
AE_NOT_ADDABLE()
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(ParallaxComponent, "Parallax", "Rendering", ICON_FA_LAYER_GROUP)
AE_FIELD_N("factor", factor, Vec2)
AE_FIELD_N("scroll_speed", scrollSpeed, Vec2)
AE_GENERIC_SERIALIZE()
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
b.SerializeKey("particles"); // legacy on-disk key predates the display-name derivation
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(LookAtComponent, "Look At", "Behaviors", ICON_FA_EYE)
AE_FIELD_N("target", target, Vec3)
AE_FIELD_N("keep_upright", keepUpright, Bool)
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(RootMotionComponent, "Root Motion", "Behaviors", ICON_FA_PERSON_RUNNING)
AE_FIELD_N("apply_to_physics", applyToPhysics, Bool)
AE_FIELD_N("apply_to_transform", applyToTransform, Bool)
AE_FIELD_N("enabled", enabled, Bool)
AE_NOT_ADDABLE()
AE_COMPONENT_END()

AE_COMPONENT(AnimationBlendComponent, "Animation Blend", "Rendering", ICON_FA_SHUFFLE)
AE_FIELD_N("primary_clip", primaryClip, UInt)
AE_FIELD_N("secondary_clip", secondaryClip, UInt)
AE_FIELD_N("blend_weight", blendWeight, Float)
AE_FIELD_N("transition_speed", transitionSpeed, Float)
AE_NOT_ADDABLE()
AE_COMPONENT_END()

// UI components: authored data (the runtime resolved-rect is not reflected). Addable via
// the MCP / editor so UI can be built through the authoring flow, and
// serialized/inspected/MCP-reachable via reflection. Canvas/Rect/Text ride the generic
// serde table; UI Image keeps its bespoke serde (texture handle <-> path) and reflects the
// path through a CustomField. Enums persist as names.
AE_COMPONENT(UiCanvasComponent, "UI Canvas", "UI", ICON_FA_WINDOW_MAXIMIZE)
AE_FIELD_ENUM("scale_mode", scaleMode, UiScaleModeEnum())
AE_FIELD_N("reference", referenceResolution, Vec2)
AE_FIELD_N("sort_bias", sortBias, Int)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiRectComponent, "UI Rect", "UI", ICON_FA_VECTOR_SQUARE)
AE_FIELD_N("anchor_min", anchorMin, Vec2)
AE_FIELD_N("anchor_max", anchorMax, Vec2)
AE_FIELD_N("offset_min", offsetMin, Vec2)
AE_FIELD_N("offset_max", offsetMax, Vec2)
AE_FIELD_N("pivot", pivot, Vec2)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiTextComponent, "UI Text", "UI", ICON_FA_FONT)
AE_FIELD_N("text", text, String)
AE_FIELD_N("font", fontName, String)
AE_FIELD_N("pixel_size", pixelSize, Float)
AE_FIELD_N("color", color, Color4)
AE_FIELD_ENUM("h_align", hAlign, UiHAlignEnum())
AE_FIELD_ENUM("v_align", vAlign, UiVAlignEnum())
AE_FIELD_N("wrap", wrap, Bool)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiImageComponent, "UI Image", "UI", ICON_FA_IMAGE)
AE_FIELD_N("color", color, Color4)
AE_FIELD_N("corner_radius", cornerRadius, Float)
AE_FIELD_CUSTOM(
        "texture", String,
        [](const void* c) -> ::aether::reflect::FieldValue
        { return ::aether::reflect::MakeValue(static_cast<const UiImageComponent*>(c)->texturePath); },
        [](void* c, const ::aether::reflect::FieldValue& v)
        {
	        auto* img = static_cast<UiImageComponent*>(c);
	        img->texturePath = v.str;
	        img->textureDirty = true;
        })
AE_FIELD_N("pixel_art", pixelArt, Bool)
AE_COMPONENT_END()

AE_COMPONENT(UiSelectableComponent, "UI Selectable", "UI", ICON_FA_HAND_POINTER)
AE_FIELD_N("group", group, String)
AE_FIELD_N("interactable", interactable, Bool)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
