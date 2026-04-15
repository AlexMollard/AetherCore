#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "Material.hpp"

namespace aether
{
	class GraphicsPipeline;
	class Mesh;

	// ── ECS component types ────────────────────────────────────────────────
	// Each is a plain-data struct; the World owns all component storage.

	// World-space transform (model matrix).
	struct TransformComponent
	{
		glm::mat4 localToWorld{ 1.0f };
	};

	// Reference to a GPU vertex/index buffer.
	struct MeshComponent
	{
		const Mesh* mesh = nullptr;
	};

	// Surface shading properties (textures, tint, …).
	struct MaterialComponent
	{
		Material material{};
	};

	// Pipeline (shader + raster state) used to draw the entity.
	struct PipelineComponent
	{
		const GraphicsPipeline* pipeline = nullptr;
	};

	// BDA of a joint-matrix palette for a skinned mesh.
	// Absent on non-skinned entities — World falls back to skinBufferAddr = 0.
	struct SkinComponent
	{
		VkDeviceAddress skinBufferAddr = 0;
	};
}
