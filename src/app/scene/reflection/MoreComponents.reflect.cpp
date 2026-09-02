#include "scene/reflection/Reflection.hpp"

#include "debug/Icons.hpp"
#include "net/NetComponents.hpp"
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
using UiTextBoxComponent = aether::ui::UITextBox;
using UiMaskComponent = aether::ui::UIMask;
using UiEffectComponent = aether::ui::UIEffect;
using UiMaterialComponent = aether::ui::UIMaterial;
using NetworkIdentityComponent = aether::net::NetworkIdentity;
using NetworkTransformComponent = aether::net::NetworkTransform;
using NetPlayerComponent = aether::net::NetPlayer;

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

	const reflect::EnumTable& UiContentTypeEnum()
	{
		static const reflect::EnumTable table{{
		        {"any", static_cast<int>(aether::ui::TextContentType::Any)},
		        {"integer", static_cast<int>(aether::ui::TextContentType::Integer)},
		        {"decimal", static_cast<int>(aether::ui::TextContentType::Decimal)},
		        {"alphanumeric", static_cast<int>(aether::ui::TextContentType::Alphanumeric)},
		        {"host", static_cast<int>(aether::ui::TextContentType::Host)},
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
AE_FIELD_NT("sorting_layer", sortingLayer, Int, "Coarse draw order against other 2D content - higher draws in front. It is compared before order_in_layer, so a higher sorting_layer wins however low its order.")
AE_FIELD_NT("order_in_layer", orderInLayer, Int, "Fine draw order within the same sorting_layer, higher in front. Ties fall back to entity id - stable, but not something to depend on.")
AE_FIELD_NT("visible_layers", visibleLayerMask, UInt, "A bitmask, one bit per tile layer, not a count: 1 shows only the first layer, 3 the first two, 0xFFFFFFFF everything. Layers past the 32nd cannot be masked and always draw.")
AE_FIELD_N("visible", visible, Bool)
b.RequiresFeature(SceneFeatureFlags::Tilemaps);
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(DayNightComponent, "Day Night", "Rendering", ICON_FA_CLOUD_SUN)
AE_FIELD_NT("animate", animate, Bool, "Advance the clock on its own. Off freezes time_of_day where it is, which is what you want while dressing a scene.")
AE_FIELD_NT("time_of_day", timeOfDayHours, Float, "Hours on a 24-hour clock, wrapped: 6 is dawn, 12 noon, 18 dusk. Only the FIRST enabled Day Night component in the scene drives anything - a second one is ignored.")
AE_FIELD_NT("time_speed", timeSpeedSecondsPerSecond, Float, "In-world seconds per real second, so 60 makes a day last 24 minutes and 3600 makes an hour pass every second. Clamped to 0.01 - 86400.")
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
AE_FIELD_NT("factor", factor, Vec2, "How much this layer lags the camera, per axis. 0 pins it to the camera so it never appears to move - a far horizon; 1 leaves it fixed in the world and it sweeps past at full speed, like anything on the gameplay plane. Distant layers want small numbers.")
AE_FIELD_NT("scroll_speed", scrollSpeed, Vec2, "Constant drift in units per second, added on top of the camera lag. This is what keeps clouds moving while the camera stands still.")
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(ParticleEmitterComponent, "Particle Emitter", "Rendering", ICON_FA_WAND_MAGIC_SPARKLES)
AE_FIELD_N("texture", texturePath, String)
AE_FIELD_NT("rate", rate, Float, "Continuous particles per second (0 = burst only).")
AE_FIELD_NT("burst_count", burstCount, UInt, "How many particles a single burst releases at once, on top of whatever rate is producing.")
AE_FIELD_NT("emit_on_start", emitOnStart, Bool, "Fire the burst once when the emitter appears.")
AE_FIELD_NT("emitting", emitting, Bool, "Spawn new particles at the rate above. Turning it off lets the ones already alive finish rather than cutting them.")
AE_FIELD_NT("auto_destroy", autoDestroyWhenDone, Bool, "One-shot effect entities retire themselves once the last particle dies.")
AE_FIELD_N("max_particles", maxParticles, UInt)
AE_FIELD_N("lifetime_min", lifetimeMin, Float)
AE_FIELD_N("lifetime_max", lifetimeMax, Float)
AE_FIELD_N("speed_min", speedMin, Float)
AE_FIELD_N("speed_max", speedMax, Float)
AE_FIELD_NT("direction_deg", directionDeg, Float, "Which way particles are thrown, in degrees. 90 is straight up.")
AE_FIELD_NT("spread_deg", spreadDeg, Float, "Scatter either side of that direction, so the cone is twice this wide: 30 gives a 60-degree fan, 180 emits in every direction.")
AE_FIELD_N("gravity", gravity, Vec2)
AE_FIELD_NT("start_size", startSize, Float, "Size at birth. Each particle keeps its own jitter, so this is the average rather than an exact width.")
AE_FIELD_NT("end_size", endSize, Float, "Size at death, interpolated from start_size across the particle's life. Smaller than start_size shrinks, larger grows.")
AE_FIELD_N("start_color", startColor, Color4)
AE_FIELD_N("end_color", endColor, Color4)
AE_FIELD_ENUM("blend_mode", blendMode, ParticleBlendEnum())
AE_FIELD_NT("sorting_layer", sortingLayer, Int, "Coarse draw order against other 2D content - higher draws in front. It is compared before order_in_layer, so a higher sorting_layer wins however low its order.")
AE_FIELD_NT("order_in_layer", orderInLayer, Int, "Fine draw order within the same sorting_layer, higher in front. Ties fall back to entity id - stable, but not something to depend on.")
AE_FIELD_NT("collide_world", collideWorld, Bool, "Bounce off physics colliders. Only runs in a live scene, not in the Particles panel preview.")
AE_FIELD_NT("collide_particles", collideParticles, Bool, "Particles bounce off one another.")
AE_FIELD_N("bounce", bounce, Float)
AE_FIELD_NT("collision_damping", collisionDamping, Float, "Tangential speed lost on a world hit - how much a particle is slowed as it scrapes along a surface, separately from how much it bounces.")
AE_FIELD_NT("collision_radius", collisionRadius, Float, "0 = derive from particle size.")
b.SerializeKey("particles"); // legacy on-disk key predates the display-name derivation
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(LookAtComponent, "Look At", "Behaviors", ICON_FA_EYE)
AE_FIELD_NT("target", target, Vec3, "The world-space point to face. It is a position, not a direction or an entity - move it and the entity turns to follow.")
AE_FIELD_NT("keep_upright", keepUpright, Bool, "Roll-free: keep world up as the up axis so the entity only yaws and pitches. Looking within a few degrees of straight up or down falls back to the Z axis, because there is no upright answer there.")
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(RootMotionComponent, "Root Motion", "Behaviors", ICON_FA_PERSON_RUNNING)
AE_FIELD_NT("apply_to_physics", applyToPhysics, Bool, "Reserved: nothing reads this yet, so it has no effect. Root motion is currently read from script via the accumulated delta.")
AE_FIELD_NT("apply_to_transform", applyToTransform, Bool, "Reserved: nothing reads this yet, so it has no effect. Root motion is currently read from script via the accumulated delta.")
AE_FIELD_NT("enabled", enabled, Bool, "Whether root motion accumulates at all. Scripts read and set this through the animation API.")
AE_NOT_ADDABLE()
AE_COMPONENT_END()

AE_COMPONENT(AnimationBlendComponent, "Animation Blend", "Rendering", ICON_FA_SHUFFLE)
AE_FIELD_NT("primary_clip", primaryClip, UInt, "The clip currently playing. When a transition finishes, the secondary clip takes this slot.")
AE_FIELD_NT("secondary_clip", secondaryClip, UInt, "The clip being blended toward. Only meaningful during a transition; it is cleared once the blend completes.")
AE_FIELD_NT("blend_weight", blendWeight, Float, "How much of the primary clip is still showing: it counts DOWN from 1 to 0 across a transition, and reaching 0 is what promotes the secondary clip and ends the blend.")
AE_FIELD_NT("transition_speed", transitionSpeed, Float, "Weight lost per second, so a transition lasts 1 divided by this: 4 gives a quarter-second crossfade, 1 a full second.")
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
AE_FIELD_NT("anchor_min", anchorMin, Vec2, "Where this rect's top-left corner sits in its parent, as a fraction: 0,0 is the parent's TOP-left and 1,1 its bottom-right. Y counts downward, which is the opposite of the world axis and the usual first surprise here.")
AE_FIELD_NT("anchor_max", anchorMax, Vec2, "The same fraction for the bottom-right corner. Setting it equal to anchor_min pins a fixed-size rect to one spot; spreading them apart (0,0 to 1,1) makes the rect stretch with its parent.")
AE_FIELD_NT("offset_min", offsetMin, Vec2, "Pixels added to the top-left anchor. With both anchors on the same spot this is position; with them spread apart it is a margin from the parent's edge.")
AE_FIELD_NT("offset_max", offsetMax, Vec2, "Pixels added to the bottom-right anchor. Positive values push it further right and down, so a margin inside the parent needs a negative one.")
AE_FIELD_NT("pivot", pivot, Vec2, "Which point of the rect a size change grows from - 0.5,0.5 grows evenly, 0,0 grows right and down. It is baked into the offsets as you resize; the layout itself only reads the anchors and offsets.")
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
AE_FIELD_NT("group", group, String, "Scopes arrow-key navigation: a selectable with a group only moves focus to others sharing it, which keeps two clusters of controls on one screen from stealing focus from each other. Leave it empty to reach everything. Tab still cycles the whole screen, so it is the way out of a group.")
AE_FIELD_NT("interactable", interactable, Bool, "Off takes this out of keyboard and gamepad navigation entirely, which is how a locked or unavailable entry is skipped rather than focused and refused.")
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

AE_COMPONENT(UiTextBoxComponent, "UI Text Box", "UI", ICON_FA_KEYBOARD)
AE_FIELD_N("text", text, String)
AE_FIELD_N("placeholder", placeholder, String)
AE_FIELD_N("font", fontName, String)
AE_FIELD_ENUM("content_type", contentType, UiContentTypeEnum())
AE_FIELD_N("allowed_chars", allowedChars, String)
AE_FIELD_N("max_length", maxLength, Int)
AE_FIELD_N("password", password, Bool)
AE_FIELD_N("pixel_size", pixelSize, Float)
AE_FIELD_N("corner_radius", cornerRadius, Float)
AE_FIELD_N("padding", padding, Float)
AE_FIELD_N("bg_color", bgColor, Color4)
AE_FIELD_N("bg_color_focused", bgColorFocused, Color4)
AE_FIELD_N("text_color", textColor, Color4)
AE_FIELD_N("placeholder_color", placeholderColor, Color4)
AE_FIELD_N("caret_color", caretColor, Color4)
AE_FIELD_N("selection_color", selectionColor, Color4)
b.PostSet([](World& w, Entity e) { EnsureWidgetCompanions(w, e, true); });
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(UiMaskComponent, "UI Mask", "UI", ICON_FA_CROP)
AE_FIELD_N("enabled", enabled, Bool)
AE_FIELD_N("padding", padding, Float)
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

// Deliberately reflects NO fields: netId/owner/spawnPrefab/scenePlaced are all runtime
// state a session assigns, and persisting them would fight the deterministic
// scene-placed id derivation (NetSpawn.cpp). AE_GENERIC_SERIALIZE is still required -
// without it all three serializer paths skip the type entirely and the component's
// PRESENCE does not survive a save/load, so AssignScenePlacedNetIds would find nothing
// outside tests that emplace it programmatically. A zero-field generic component
// round-trips as an empty table, which is exactly the "this entity is replicated" mark
// this component is.
AE_COMPONENT(NetworkIdentityComponent, "Network Identity", "Networking", ICON_FA_TOWER_BROADCAST)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(NetworkTransformComponent, "Network Transform", "Networking", ICON_FA_ARROWS_LEFT_RIGHT)
AE_FIELD_NT("interpolation_delay", interpolationDelaySeconds, Float, "How far in the past a REMOTE entity is rendered, in seconds, so there is a packet either side of the render time to interpolate between. Raising it smooths motion over a worse connection at the cost of lag. It does nothing to a locally-owned entity: the owner is authoritative and takes no correction.")
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()

AE_COMPONENT(NetPlayerComponent, "Net Player", "Networking", ICON_FA_USER)
AE_FIELD_REP("display_name", displayName, String)
AE_FIELD_REP("ping_ms", pingMs, UInt)
AE_GENERIC_SERIALIZE()
AE_COMPONENT_END()
