#pragma once

// This header is included by every das module .cpp file.
// It provides DasEntity, the glm/das conversion helpers, and the SceneContext
// accessor - all without pulling in daScript's heavy headers (those only appear
// in the .cpp files that actually need them).

#include <cstdint>
#include <glm/glm.hpp>

#include "scripting/SceneContext.hpp"

// ── DasEntity ─────────────────────────────────────────────────────────────────

// Lightweight entity reference used in scripts.
// Passed around by value; methods resolve the world via the TLS context.
struct DasEntity
{
	uint32_t id = 0;

	[[nodiscard]] bool IsValid() const
	{
		return id != 0;
	}
};

// ── float3 ↔ glm::vec3 conversion ─────────────────────────────────────────────

// daScript's float3 is stored as a float4 (w = 0, xyz = the vector).
// We use this simple struct to receive float3 args in binding functions.
struct DasFloat3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 0.0f; // padding/ignored

	[[nodiscard]] glm::vec3 ToGlm() const
	{
		return { x, y, z };
	}
};

// CameraHandle is just a uint32 - pass it around in scripts as a plain uint.
// Scripts use: let cam = create_camera(...); set_main_camera(cam);
using DasCameraHandle = uint32_t;
