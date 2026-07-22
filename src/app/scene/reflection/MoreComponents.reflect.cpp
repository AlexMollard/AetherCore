#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "particles/ParticleComponents.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/World.hpp"
#include "ui/UiComponents.hpp"

using namespace aether;

// AE_COMPONENT pastes the type token, so it needs an unqualified name; alias the
// namespaced UI components (and reach their nested enums through the alias).
using UiCanvasComponent = aether::ui::UICanvas;
using UiRectComponent = aether::ui::UIRect;
using UiTextComponent = aether::ui::UIText;
using UiImageComponent = aether::ui::UIImage;
using UiSelectableComponent = aether::ui::UISelectable;
using UiSliderComponent = aether::ui::UISlider;
using UiToggleComponent = aether::ui::UIToggle;
using UiButtonComponent = aether::ui::UIButton;
using UiProgressBarComponent = aether::ui::UIProgressBar;
using UiEffectComponent = aether::ui::UIEffect;
using UiMaterialComponent = aether::ui::UIMaterial;

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

	// Interactive widgets are self-contained: adding one composes the UIRect it lays out in
	// and (when interactive) the UISelectable the nav system drives. Idempotent, so it is safe
	// to run on every add/set/apply.
	void EnsureWidgetCompanions(World& world, Entity e, bool selectable)
	{
		if (!world.Has<aether::ui::UIRect>(e))
		{
			auto& r = world.Emplace<aether::ui::UIRect>(e);
			r.anchorMin = {0.5f, 0.5f};
			r.anchorMax = {0.5f, 0.5f};
			r.offsetMin = {-110.f, -14.f};
			r.offsetMax = {110.f, 14.f};
		}
		if (selectable && !world.Has<aether::ui::UISelectable>(e))
		{
			world.Emplace<aether::ui::UISelectable>(e);
		}
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

AE_COMPONENT(UiSliderComponent, "UI Slider", "UI", ICON_FA_SLIDERS)
AE_FIELD_N("min", minValue, Float)
AE_FIELD_N("max", maxValue, Float)
AE_FIELD_N("step", step, Float)
AE_FIELD_N("value", value, Float)
AE_FIELD_N("track_color", trackColor, Color4)
AE_FIELD_N("fill_color", fillColor, Color4)
AE_FIELD_N("handle_color", handleColor, Color4)
AE_FIELD_N("handle_radius", handleRadius, Float)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiToggleComponent, "UI Toggle", "UI", ICON_FA_TOGGLE_ON)
AE_FIELD_N("on", on, Bool)
AE_FIELD_N("track_color", trackColor, Color4)
AE_FIELD_N("on_color", onColor, Color4)
AE_FIELD_N("knob_color", knobColor, Color4)
AE_FIELD_N("knob_radius", knobRadius, Float)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiButtonComponent, "UI Button", "UI", ICON_FA_SQUARE)
AE_FIELD_N("label", label, String)
AE_FIELD_N("font", fontName, String)
AE_FIELD_N("pixel_size", pixelSize, Float)
AE_FIELD_ENUM("h_align", hAlign, UiHAlignEnum())
AE_FIELD_ENUM("v_align", vAlign, UiVAlignEnum())
AE_FIELD_N("bg_color", bgColor, Color4)
AE_FIELD_N("text_color", textColor, Color4)
AE_FIELD_N("bg_color_focused", bgColorFocused, Color4)
AE_FIELD_N("text_color_focused", textColorFocused, Color4)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiProgressBarComponent, "UI Progress Bar", "UI", ICON_FA_BARS_PROGRESS)
AE_FIELD_N("value", value, Float)
AE_FIELD_N("track_color", trackColor, Color4)
AE_FIELD_N("fill_color", fillColor, Color4)
AE_FIELD_N("corner_radius", cornerRadius, Float)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, false); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiEffectComponent, "UI Effect", "UI", ICON_FA_WAND_MAGIC_SPARKLES)
AE_FIELD_N("shader", shader, String)
AE_FIELD_N("color0", color0, Color4)
AE_FIELD_N("color1", color1, Color4)
AE_FIELD_N("background", background, Bool)
AE_FIELD_N("sort_order", sortOrder, Int)
b.PostSet(
        [](World& w, Entity e)
        {
	        // Effects are full-screen quads; ensure a stretched UIRect so a bare add just works.
	        if (!w.Has<aether::ui::UIRect>(e))
	        {
		        auto& r = w.Emplace<aether::ui::UIRect>(e);
		        r.anchorMin = {0.f, 0.f};
		        r.anchorMax = {1.f, 1.f};
		        r.offsetMin = {0.f, 0.f};
		        r.offsetMax = {0.f, 0.f};
	        }
        });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

// A custom fragment shader on the element's own draw commands (its glyphs/image/rect), masked to
// their shapes - unlike UI Effect which is a separate quad. Add it to a text/image/rect element.
AE_COMPONENT(UiMaterialComponent, "UI Material", "UI", ICON_FA_WAND_MAGIC_SPARKLES)
AE_FIELD_N("shader", shader, String)
AE_FIELD_N("color0", color0, Color4)
AE_FIELD_N("color1", color1, Color4)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
