#pragma once

#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/Components.hpp"
#include "scene/Entity.hpp"
#include "scene/World.hpp"

namespace aether
{
	// propagation carries them) and serialize like any other entity.
	struct PointLightComponent
	{
		glm::vec3 color{1.0f};
		float intensity = 20.0f;
		float radius = 15.0f;
		// Physical size of the emitter, in world units. Drives PCSS penumbra width:
		// a bare filament throws razor shadows, a softbox throws broad ones. Not the
		// falloff radius above, which is how far the light reaches.
		float sourceRadius = 0.1f;
		bool castsShadow = false;
		// Flame-style flicker: intensity dips by up to `flicker` [0..1] at `flickerSpeed` (Hz-ish),
		// with an organic per-light phase. 0 = steady. Applied on the CPU so it affects 2D and 3D alike.
		float flicker = 0.0f;
		float flickerSpeed = 8.0f;
	};

	struct SpotLightComponent
	{
		glm::vec3 color{1.0f};
		float intensity = 30.0f;
		float radius = 30.0f;
		float innerAngleRad = 0.35f;
		float outerAngleRad = 0.60f;
		float sourceRadius = 0.1f;
		bool castsShadow = false;
		float flicker = 0.0f;
		float flickerSpeed = 8.0f;
	};

	// Per-scene 2D lighting settings for the screen-space light map. The first enabled instance in a
	// scene drives ambient (the dark floor everything sinks toward) and the global shadow look. Absent =
	// engine defaults, so scenes without one behave exactly as before.
	struct Light2DSettingsComponent
	{
		glm::vec3 ambientColor{0.03f, 0.04f, 0.06f};
		float ambientIntensity = 1.0f; // scales ambientColor
		float shadowStrength = 0.94f;  // 0 = no shadows, 1 = fully black
		float shadowSoftness = 1.0f;   // penumbra width multiplier
		// Light scattered by the air between the pixel and the light, which is what makes a
		// beam visible in the gap between two occluders. 0 is off, and it is off by default:
		// it is a look, and a look nobody asked for is a regression.
		float shaftStrength = 0.0f;
		// How much the air's density varies from place to place. 0 is a perfectly uniform
		// haze; higher values break it into drifting dust and mist, which is what stops a
		// lit room reading as a flat wash of fog.
		float shaftNoise = 0.0f;
	};

	// Animated sun/sky environment driver. Scene content, not a global: the
	// DayNightSystem drives the renderer from the first enabled instance and
	// leaves the authored environment untouched when no entity carries one, so
	// 2D scenes (no Lighting3D feature) never fight it. Time state lives here
	// so it serializes and play-restores like any other authored data.
	// Authored procedural sky. Without this the gradient could only be reached by
	// hand-editing the scene's [environment] block or by adding a Day/Night cycle,
	// which meant a scene that just wants a fixed sky had no way to say so.
	// A Day/Night component on any entity drives the sky itself and takes precedence.
	struct SkyComponent
	{
		glm::vec3 horizonColor{0.34f, 0.52f, 0.82f};
		glm::vec3 zenithColor{0.08f, 0.19f, 0.45f};
		glm::vec3 groundColor{0.001f, 0.002f, 0.005f};
		glm::vec3 ambientColor{0.03f, 0.04f, 0.06f};

		// Atmospheric perspective. Without it distant geometry keeps the same contrast
		// and saturation as anything near the camera, which is the single strongest cue
		// that a scene is not a place. Density is per world unit at y = 0 and thins
		// exponentially with height at fogHeightFalloff.
		float fogDensity{0.0f};
		float fogHeightFalloff{0.08f};
		// Forward scattering toward the sun, which is what makes haze glow when you
		// look into it and stay flat when you look away.
		float fogSunScatter{0.6f};
		// Ceiling on fog opacity, so the far plane never becomes a flat wall of colour.
		float fogMaxOpacity{0.9f};

		// Cloud cover over the visible sky. Weather sits with the rest of the atmosphere
		// rather than in graphics settings: one game holds a clear level and an overcast
		// one. Coverage 0 costs nothing - the shader returns before sampling any noise.
		float cloudCoverage{0.0f};
		float cloudSpeed{0.02f};

		// Swap the authored horizon-to-zenith gradient for single-scattering atmosphere.
		// Off by default: the gradient is what every existing scene was authored
		// against, and turning this on globally would restyle all of them.
		bool physicalSky{false};
		// Haze. Low is a clear high-altitude sky, high is humid or dusty air - it
		// widens the sun's halo and washes the horizon toward white.
		float turbidity{2.5f};

		// Solve the fog above by marching the sun's shadow cascades instead of by the
		// closed form. It is the same integral - light scattered toward the eye, plus
		// what survived the air - but the march knows which parts of the air the sun
		// actually reaches, so shadows carve beams out of the haze. Air only looks like
		// a volume once something can block it.
		//
		// Off by default, and it REPLACES the analytic fog rather than adding to it:
		// running both would count the same scattering twice.
		bool volumetricFog{false};
		// Scales the scattered light alone. The extinction that dims distant geometry
		// stays tied to fogDensity, so this brightens the beams without quietly
		// changing how far you can see.
		float volumetricIntensity{1.0f};
		// Mie asymmetry. 0 scatters evenly, and toward 1 the air throws light forward,
		// which is what makes a beam blaze when you look into the sun and fade as you
		// turn away.
		float volumetricAnisotropy{0.6f};
	};

	struct DayNightComponent
	{
		bool animate = true;
		float timeOfDayHours = 6.0f;             // wraps [0, 24)
		float timeSpeedSecondsPerSecond = 60.0f; // simulated seconds per real second
	};

	namespace ecs
	{
		inline glm::mat4 LightAimMatrix(const glm::vec3& position, const glm::vec3& direction)
		{
			const glm::vec3 dir = glm::normalize(direction);
			const glm::vec3 up = glm::abs(glm::dot(dir, glm::vec3(0, 1, 0))) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			glm::mat4 m = glm::mat4_cast(glm::quatLookAt(dir, up));
			m[3] = glm::vec4(position, 1.0f);
			return m;
		}

		inline Entity CreatePointLightEntity(World& world, const glm::vec3& position, const PointLightComponent& light, std::string name = "Point Light")
		{
			const Entity e = world.Create();
			world.Emplace<NameComponent>(e, NameComponent{.name = std::move(name)});
			glm::mat4 m{1.0f};
			m[3] = glm::vec4(position, 1.0f);
			world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = m});
			world.Emplace<PointLightComponent>(e, light);
			return e;
		}

		inline Entity CreateSpotLightEntity(World& world, const glm::vec3& position, const glm::vec3& direction, const SpotLightComponent& light, std::string name = "Spot Light")
		{
			const Entity e = world.Create();
			world.Emplace<NameComponent>(e, NameComponent{.name = std::move(name)});
			world.Emplace<TransformComponent>(e, TransformComponent{.localToWorld = LightAimMatrix(position, direction)});
			world.Emplace<SpotLightComponent>(e, light);
			return e;
		}
	} // namespace ecs
} // namespace aether
