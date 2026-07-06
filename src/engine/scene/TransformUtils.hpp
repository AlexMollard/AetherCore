#pragma once

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace aether
{
	// Compose a TRS matrix from pos / euler(degrees) / scale - YXZ rotation order.
	inline glm::mat4 ComposeTransform(glm::vec3 pos, glm::vec3 rotEulerDeg, glm::vec3 scale)
	{
		glm::mat4 t = glm::translate(glm::mat4(1.0f), pos);
		glm::mat4 r = glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.y), glm::vec3(0, 1, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.x), glm::vec3(1, 0, 0));
		r = r * glm::rotate(glm::mat4(1.0f), glm::radians(rotEulerDeg.z), glm::vec3(0, 0, 1));
		glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
		return t * r * s;
	}

	// Extract YXZ euler angles (degrees) + per-axis scale from a TRS matrix.
	inline void DecomposeTRS(const glm::mat4& m, glm::vec3& pos, glm::vec3& eulerDeg, glm::vec3& scale)
	{
		pos = glm::vec3(m[3]);
		float sx = glm::length(glm::vec3(m[0]));
		float sy = glm::length(glm::vec3(m[1]));
		float sz = glm::length(glm::vec3(m[2]));
		scale = {sx, sy, sz};
		glm::vec3 c2 = sz > 1e-6f ? glm::vec3(m[2]) / sz : glm::vec3(0, 0, 1);
		glm::vec3 c0 = sx > 1e-6f ? glm::vec3(m[0]) / sx : glm::vec3(1, 0, 0);
		float sinX = glm::clamp(-c2.y, -1.0f, 1.0f);
		float rotXRad = std::asin(sinX);
		float rotYRad = 0.0f;
		float rotZRad = 0.0f;
		float cosX = std::cos(rotXRad);
		if (std::abs(cosX) > 1e-4f)
		{
			glm::vec3 c1 = sy > 1e-6f ? glm::vec3(m[1]) / sy : glm::vec3(0, 1, 0);
			rotYRad = std::atan2(c2.x, c2.z);
			rotZRad = std::atan2(c0.y, c1.y);
		}
		else
		{
			// Gimbal lock (X = ±90°): Y and Z merge and column 2 no longer carries
			// Y. Recover the merged angle from column 0 - (cos(Y∓Z), 0, ∓sin(Y∓Z))
			// at X = ±90° - and fold it into Y with Z := 0 so recomposition still
			// reproduces the matrix.
			rotYRad = std::atan2(-c0.z, c0.x);
		}
		eulerDeg = {glm::degrees(rotXRad), glm::degrees(rotYRad), glm::degrees(rotZRad)};
	}
} // namespace aether
