#pragma once

#include <cmath>
#include <optional>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace aether
{
	inline glm::mat4 ComposeTransform(glm::vec3 pos, glm::vec3 rotEulerDeg, glm::vec3 scale)
	{
		const glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
		glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.y), glm::vec3(0, 1, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.x), glm::vec3(1, 0, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.z), glm::vec3(0, 0, 1));
		const glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
		return t * r * s;
	}

	// Per-axis scale = the length of each basis column. This is the correct way to
	// recover scale from a TRS matrix (m[i][i] is wrong once there is any rotation).
	inline glm::vec3 ExtractScale(const glm::mat4& m)
	{
		return {glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2]))};
	}

	inline void DecomposeTRS(const glm::mat4& m, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
	{
		pos = glm::vec3(m[3]);
		scale = ExtractScale(m);
		const float sx = scale.x;
		const float sy = scale.y;
		const float sz = scale.z;
		const glm::vec3 c2 = sz > 1e-6f ? glm::vec3(m[2]) / sz : glm::vec3(0, 0, 1);
		const glm::vec3 c0 = sx > 1e-6f ? glm::vec3(m[0]) / sx : glm::vec3(1, 0, 0);
		const float sinX = glm::clamp(-c2.y, -1.0f, 1.0f);
		const float rotXRad = std::asin(sinX);
		float rotYRad = 0.0f;
		float rotZRad = 0.0f;
		const float cosX = std::cos(rotXRad);
		if (std::abs(cosX) > 1e-4f)
		{
			const glm::vec3 c1 = sy > 1e-6f ? glm::vec3(m[1]) / sy : glm::vec3(0, 1, 0);
			rotYRad = std::atan2(c2.x, c2.z);
			rotZRad = std::atan2(c0.y, c1.y);
		}
		else
		{
			// Gimbal lock (X = ±90°): Y and Z merge and column 2 no longer carries
			rotYRad = std::atan2(-c0.z, c0.x);
		}
		eulerDeg = {glm::degrees(rotXRad), glm::degrees(rotYRad), glm::degrees(rotZRad)};
	}

	struct TransformDelta
	{
		glm::vec3 position{0.0f};
		glm::vec3 eulerDeg{0.0f};
		glm::vec3 scale{0.0f};
	};

	inline glm::mat4 ApplyTransformDelta(const glm::mat4& target, const TransformDelta& delta)
	{
		glm::vec3 pos{}, euler{}, scale{};
		DecomposeTRS(target, pos, euler, scale);
		pos += delta.position;
		euler += delta.eulerDeg;
		scale = glm::max(scale + delta.scale, glm::vec3(0.001f));
		return ComposeTransform(pos, euler, scale);
	}
	// A partial transform edit: whichever parts are set replace the matching part of
	// `current`, and the rest are taken from it unchanged. Composing from scratch with
	// identity defaults instead is silent data loss - setting a position that way throws
	// away the rotation and scale nobody mentioned.
	struct PartialTransform
	{
		std::optional<glm::vec3> position;
		std::optional<glm::vec3> eulerDeg;
		std::optional<glm::vec3> scale;
	};

	inline glm::mat4 ComposeTransformOver(const glm::mat4& current, const PartialTransform& parts)
	{
		glm::vec3 pos{};
		glm::vec3 euler{};
		glm::vec3 scale{};
		DecomposeTRS(current, pos, euler, scale);
		return ComposeTransform(parts.position.value_or(pos), parts.eulerDeg.value_or(euler), parts.scale.value_or(scale));
	}
} // namespace aether
