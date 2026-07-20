// preserves the on-disk format. Runtime/cached fields are omitted by not listing

#include "scene/reflection/Reflection.hpp"

#include <algorithm>
#include <charconv>

#include "assets/SpriteAtlasAsset.hpp"
#include "debug/Icons.hpp"
#include "scene/BehaviorComponents.hpp"
#include "scene/CameraComponents.hpp"
#include "scene/Components.hpp"
#include "scene/LightComponents.hpp"
#include "scene/TransformUtils.hpp"

using namespace aether;
using aether::reflect::FieldType;
using aether::reflect::FieldValue;

namespace
{
	const reflect::EnumTable& CameraProjectionEnum()
	{
		static const reflect::EnumTable table{{{"perspective", static_cast<int>(CameraProjection::Perspective)}, {"orthographic", static_cast<int>(CameraProjection::Orthographic)}}};
		return table;
	}

	const reflect::EnumTable& CameraBackgroundEnum()
	{
		static const reflect::EnumTable table{{
		        {"solid_colour", static_cast<int>(CameraBackground::SolidColour)},
		        {"gradient", static_cast<int>(CameraBackground::Gradient)},
		        {"sky_gradient", static_cast<int>(CameraBackground::SkyGradient)},
		}};
		return table;
	}

	const reflect::EnumTable& SpriteBlendEnum()
	{
		static const reflect::EnumTable table{{
		        {"alpha", static_cast<int>(SpriteBlendMode::Alpha)},
		        {"additive", static_cast<int>(SpriteBlendMode::Additive)},
		        {"multiply", static_cast<int>(SpriteBlendMode::Multiply)},
		        {"opaque", static_cast<int>(SpriteBlendMode::Opaque)},
		}};
		return table;
	}

	const reflect::EnumTable& SpriteAnimationLoopEnum()
	{
		static const reflect::EnumTable table{{
		        {"loop", static_cast<int>(SpriteAnimationLoopMode::Loop)},
		        {"once", static_cast<int>(SpriteAnimationLoopMode::Once)},
		        {"ping-pong", static_cast<int>(SpriteAnimationLoopMode::PingPong)},
		        {"hold", static_cast<int>(SpriteAnimationLoopMode::Hold)},
		}};
		return table;
	}
} // namespace

AE_COMPONENT(SpriteRendererComponent, "Sprite Renderer", "Rendering", ICON_FA_IMAGE)
AE_FIELD_N("texture", texturePath, String)
AE_FIELD_N("atlas", atlasPath, String)
// Agent-friendly atlas sprite selection by NAME or numeric index, resolved
// against the component's atlas. Reads back as the sprite's name. Not
// serialized: scenes persist the stable sprite_id through the bespoke sprite
// TOML instead.
b.CustomField(
        "sprite", ::aether::reflect::FieldType::String,
        [](const void* component) -> ::aether::reflect::FieldValue
        {
	        const auto* sprite = static_cast<const SpriteRendererComponent*>(component);
	        std::string name;
	        if (sprite->spriteId.IsValid() && !sprite->atlasPath.empty())
	        {
		        if (const auto atlas = SpriteAtlasAsset::Load(sprite->atlasPath); atlas.has_value())
		        {
			        if (const SpriteRegion* region = atlas->Find(sprite->spriteId))
			        {
				        name = region->name;
			        }
		        }
	        }
	        return ::aether::reflect::MakeValue(name);
        },
        [](void* component, const ::aether::reflect::FieldValue& value)
        {
	        auto* sprite = static_cast<SpriteRendererComponent*>(component);
	        if (value.str.empty())
	        {
		        sprite->spriteId = {};
		        return;
	        }
	        if (sprite->atlasPath.empty())
	        {
		        return; // nothing to resolve against; set 'atlas' first
	        }
	        const auto atlas = SpriteAtlasAsset::Load(sprite->atlasPath);
	        if (!atlas.has_value())
	        {
		        return;
	        }
	        const SpriteRegion* region = nullptr;
	        std::size_t index = 0;
	        const auto [ptr, ec] = std::from_chars(value.str.data(), value.str.data() + value.str.size(), index);
	        if (ec == std::errc{} && ptr == value.str.data() + value.str.size())
	        {
		        region = index < atlas->sprites.size() ? &atlas->sprites[index] : nullptr;
	        }
	        else
	        {
		        for (const SpriteRegion& candidate: atlas->sprites)
		        {
			        if (candidate.name == value.str)
			        {
				        region = &candidate;
				        break;
			        }
		        }
	        }
	        if (region == nullptr)
	        {
		        return; // unknown sprite: leave the current selection untouched
	        }
	        sprite->spriteId = region->id;
	        // Mirror the region's geometry so the component is coherent even
	        // before the sprite system re-derives it during extraction.
	        sprite->uvRect = region->uvRect;
	        sprite->pixelSize = region->pixelSize;
	        sprite->pivot = region->pivot;
	        sprite->pixelsPerUnit = atlas->pixelsPerUnit;
	        sprite->texturePath = atlas->texturePath;
        },
        ::aether::reflect::FieldMeta{.serialize = false, .tooltip = "Atlas sprite by name or index (requires 'atlas')"});
AE_FIELD_N("uv_rect", uvRect, Vec4)
AE_FIELD_N("tint", tint, Color4)
AE_FIELD_N("pixel_size", pixelSize, Vec2)
AE_FIELD_N("pivot", pivot, Vec2)
AE_FIELD_N("pixels_per_unit", pixelsPerUnit, Float)
AE_FIELD_N("sorting_layer", sortingLayer, Int)
AE_FIELD_N("order_in_layer", orderInLayer, Int)
AE_FIELD_ENUM("blend_mode", blendMode, SpriteBlendEnum())
AE_FIELD_N("visible", visible, Bool)
AE_FIELD_N("flip_x", flipX, Bool)
AE_FIELD_N("flip_y", flipY, Bool)
AE_FIELD_N("pixel_snap", pixelSnap, Bool)
AE_FIELD_N("pixel_art", pixelArt, Bool)
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(SpriteAnimatorComponent, "Sprite Animator", "Animation", ICON_FA_FILM)
AE_FIELD_N("animation", animationPath, String)
AE_FIELD_N("speed", speed, Float)
AE_FIELD_N("start_frame", startFrame, UInt)
AE_FIELD_ENUM("loop_mode", loopMode, SpriteAnimationLoopEnum())
AE_FIELD_N("use_asset_loop_mode", useAssetLoopMode, Bool)
AE_FIELD_N("autoplay", autoplay, Bool)
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(PointLightComponent, "Point Light", "Rendering", ICON_FA_LIGHTBULB)
AE_FIELD_N("color", color, Color3)
AE_FIELD_R(intensity, Float, 0.0f, 1000.0f)
AE_FIELD_R(radius, Float, 0.0f, 500.0f)
AE_FIELD_N("shadow", castsShadow, Bool)
b.RequiresFeature(SceneFeatureFlags::Lighting3D);
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(SpotLightComponent, "Spot Light", "Rendering", ICON_FA_LIGHTBULB)
AE_FIELD_N("color", color, Color3)
AE_FIELD_R(intensity, Float, 0.0f, 1000.0f)
AE_FIELD_R(radius, Float, 0.0f, 500.0f)
AE_FIELD_ANGLE_AS("inner_angle_deg", innerAngleRad, "inner_rad")
AE_FIELD_ANGLE_AS("outer_angle_deg", outerAngleRad, "outer_rad")
AE_FIELD_N("shadow", castsShadow, Bool)
b.RequiresFeature(SceneFeatureFlags::Lighting3D);
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(CameraComponent, "Camera", "Rendering", ICON_FA_VIDEO)
AE_FIELD_ENUM("projection", projection, CameraProjectionEnum())
AE_FIELD_N("fov", fovDegrees, Float)
AE_FIELD_N("orthographic_height", orthographicHeight, Float)
AE_FIELD_N("near", nearPlane, Float)
AE_FIELD_N("far", farPlane, Float)
AE_FIELD_ENUM("background", background, CameraBackgroundEnum())
AE_FIELD_N("clear_color", clearColor, Color3)
AE_FIELD_N("gradient_angle", gradientAngleDegrees, Float)
// gradient_stops is a variable-length list of {colour, position} - the first field to
// use the reflected List type, so it is reachable via MCP/serializer instead of being
// hand-parsed. The setter re-applies the clamp/sort/min-two validation on every write.
b.CustomListField(
        "gradient_stops",
        {{"colour", FieldType::Color3}, {"position", FieldType::Float}},
        [](const void* comp) -> FieldValue
        {
	        FieldValue v;
	        v.type = FieldType::List;
	        for (const GradientStop& s: static_cast<const CameraComponent*>(comp)->gradientStops)
	        {
		        FieldValue col;
		        col.type = FieldType::Color3;
		        col.vec = glm::vec4(s.colour, 0.0f);
		        FieldValue pos;
		        pos.type = FieldType::Float;
		        pos.num = s.position;
		        v.list.push_back({col, pos});
	        }
	        return v;
        },
        [](void* comp, const FieldValue& in)
        {
	        std::vector<GradientStop>& stops = static_cast<CameraComponent*>(comp)->gradientStops;
	        stops.clear();
	        for (const std::vector<FieldValue>& row: in.list)
	        {
		        GradientStop s{};
		        if (row.size() >= 1)
		        {
			        s.colour = glm::vec3(row[0].vec);
		        }
		        if (row.size() >= 2)
		        {
			        s.position = std::clamp(static_cast<float>(row[1].num), 0.0f, 1.0f);
		        }
		        stops.push_back(s);
	        }
	        std::sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
	        if (stops.size() < 2)
	        {
		        stops = CameraComponent{}.gradientStops;
	        }
        });
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(SkinnedMeshComponent, "Skinned Mesh", "Rendering", ICON_FA_FILM)
AE_FIELD_N("clip", clipIndex, UInt)
AE_FIELD_N("time", animTime, Float)
AE_FIELD_N("speed", playbackSpeed, Float)
AE_FIELD_N("looping", looping, Bool)
AE_NOT_ADDABLE()
AE_COMPONENT_END()

AE_COMPONENT(TransformComponent, "Transform", "Core", ICON_FA_UP_DOWN_LEFT_RIGHT)
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

AE_COMPONENT(SpinComponent, "Spin", "Behaviors", ICON_FA_ROTATE)
AE_FIELD_N("euler_deg_per_sec", eulerDegPerSec, Vec3)
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(BobComponent, "Bob", "Behaviors", ICON_FA_WAVE_SQUARE)
AE_FIELD_N("amplitude", amplitude, Float)
AE_FIELD_N("frequency", frequency, Float)
AE_FIELD_N("phase", phase, Float)
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(OrbitComponent, "Orbit", "Behaviors", ICON_FA_CIRCLE_NOTCH)
AE_FIELD_N("center", center, Vec3)
AE_FIELD_N("radius", radius, Float)
AE_FIELD_N("speed_deg", angularSpeedDeg, Float)
AE_FIELD_N("angle_deg", angleDeg, Float)
AE_FIELD_N("yaw_offset_deg", yawOffsetDeg, Float)
AE_FIELD_N("height", height, Float)
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(ScalePulseComponent, "Scale Pulse", "Behaviors", ICON_FA_EXPAND)
AE_FIELD_N("amplitude", amplitude, Float)
AE_FIELD_N("frequency", frequency, Float)
AE_FIELD_N("phase", phase, Float)
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()

AE_COMPONENT(MaterialPulseComponent, "Material Pulse", "Behaviors", ICON_FA_HEART_PULSE)
AE_FIELD_N("emissive_a", emissiveA, Color3)
AE_FIELD_N("emissive_b", emissiveB, Color3)
AE_FIELD_N("frequency", frequency, Float)
AE_GENERIC_SERIALIZE()
AE_HAND_AUTHORED_CATALOG()
AE_COMPONENT_END()
