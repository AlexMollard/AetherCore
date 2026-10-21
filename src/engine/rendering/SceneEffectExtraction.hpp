#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace aether
{
	inline glm::vec4 ExtractDepthOfFieldParams(float focusDistance, float focalLengthMm, float aperture, bool enabled, bool perspective)
	{
		// Thin lens against a 24mm sensor: the shader multiplies this coefficient by
		// abs(distance-focus)/distance. The native 24-tap gather caps CoC at 24 pixels.
		constexpr float kSensorHeightMetres = 0.024f;
		const float focal = std::clamp(focalLengthMm, 1.0f, 1000.0f) * 0.001f;
		const float subjectGap = std::max(focusDistance - focal, 1e-4f);
		const float coefficient = (focal * focal) / (std::max(aperture, 0.1f) * kSensorHeightMetres * subjectGap);
		return glm::vec4(std::max(focusDistance, 1e-3f), coefficient, 24.0f, enabled && perspective ? 1.0f : 0.0f);
	}

	inline glm::vec4 TransformEffectBoundingSphere(glm::vec4 local, const glm::mat4& world)
	{
		const auto centre = glm::vec3(world * glm::vec4(glm::vec3(local), 1.0f));
		const float scale = std::max({glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2]))});
		return glm::vec4(centre, local.w * scale);
	}

	struct BlobShadowSource
	{
		std::uint32_t actorId = 0;
		glm::vec4 worldSphere{0.0f};
		bool character = false;
	};

	inline std::size_t ExtractBlobShadows(std::span<const BlobShadowSource> sources, glm::vec3 eye, std::size_t capacity, std::vector<glm::vec4>& output)
	{
		// Primitive bounds sharing an actor become one sphere-derived AABB. Characters
		// keep their contact core and budget priority; small pickups skip hover fading.
		output.clear();

		struct Bounds
		{
			glm::vec3 min{std::numeric_limits<float>::max()};
			glm::vec3 max{std::numeric_limits<float>::lowest()};
			bool character = false;
		};

		std::unordered_map<std::uint32_t, Bounds> actors;
		for (const auto& source: sources)
		{
			auto& bounds = actors[source.actorId];
			bounds.min = glm::min(bounds.min, glm::vec3(source.worldSphere) - glm::vec3(source.worldSphere.w));
			bounds.max = glm::max(bounds.max, glm::vec3(source.worldSphere) + glm::vec3(source.worldSphere.w));
			bounds.character |= source.character;
		}
		std::size_t characters = 0;
		for (const bool characterPass: {true, false})
		{
			for (const auto& [id, bounds]: actors)
			{
				if (bounds.character != characterPass)
				{
					continue;
				}
				constexpr float kFootprintFraction = 0.5f, kMinBlobRadius = 0.05f, kPickupBlobRadius = 0.4f;
				const float radius = kFootprintFraction * 0.5f * std::max(bounds.max.x - bounds.min.x, bounds.max.z - bounds.min.z);
				if (radius < kMinBlobRadius)
				{
					continue;
				}
				output.emplace_back(0.5f * (bounds.min.x + bounds.max.x), bounds.min.y, 0.5f * (bounds.min.z + bounds.max.z), radius < kPickupBlobRadius ? -radius : radius);
				characters += characterPass ? 1u : 0u;
			}
		}
		characters = std::min(characters, capacity);
		const auto distance = [&](const glm::vec4& value)
		{
			return glm::dot(glm::vec3(value) - eye, glm::vec3(value) - eye);
		};
		if (output.size() > capacity)
		{
			std::nth_element(output.begin() + characters, output.begin() + capacity, output.end(), [&](const auto& a, const auto& b) { return distance(a) < distance(b); });
			output.resize(capacity);
		}
		return characters;
	}

	struct DarknessVolumeSource
	{
		glm::mat4 world{1.0f};
		float fadeDepth = 0.0f;
	};

	inline void ExtractDarknessVolumes(std::span<const DarknessVolumeSource> sources, glm::vec3 eye, std::size_t capacity, std::vector<glm::vec4>& output)
	{
		output.clear();

		struct Volume
		{
			glm::vec4 centreFade;
			glm::vec4 halfExtentsYaw;
		};

		std::vector<Volume> volumes;
		for (const auto& source: sources)
		{
			const auto& world = source.world;
			const auto half = 0.5f * glm::vec3(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
			if (half.x <= 0.0f || half.y <= 0.0f || half.z <= 0.0f || source.fadeDepth <= 0.0f)
			{
				continue;
			}
			const float yaw = std::atan2(-world[0].z, world[0].x);
			volumes.push_back({glm::vec4(glm::vec3(world[3]), source.fadeDepth), glm::vec4(half, yaw)});
		}
		const auto distance = [&](const Volume& volume)
		{
			return glm::dot(glm::vec3(volume.centreFade) - eye, glm::vec3(volume.centreFade) - eye);
		};
		std::sort(volumes.begin(), volumes.end(), [&](const auto& a, const auto& b) { return distance(a) < distance(b); });
		volumes.resize(std::min(volumes.size(), capacity));
		for (const auto& volume: volumes)
		{
			output.push_back(volume.centreFade);
			output.push_back(volume.halfExtentsYaw);
		}
	}
} // namespace aether
