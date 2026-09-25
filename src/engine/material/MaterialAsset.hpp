#pragma once

#include <cstdint>
#include <glm/glm.hpp>

#include "material/MaterialTemplate.hpp"
#include "material/TextureHandle.hpp"

namespace aether
{
	struct MaterialAsset
	{
		glm::vec4 baseColorFactor{1.0f};
		float metallicFactor{0.0f};
		float roughnessFactor{0.5f};
		float occlusionStrength{1.0f};
		float alphaCutoff{0.5f};
		glm::vec3 emissiveFactor{0.0f};

		bool doubleSided = false;
		bool alphaBlend = false;
		bool alphaMask = false;
		bool modulateVertexColor = false;
		bool receiveShadows = true;
		// PS2 foliage/cutout card (tw-extract marks alpha-masked scenery): wrapped diffuse,
		// excluded from shadow casting/receiving.
		bool foliage = false;

		// UV scroll velocity in UV units per second (Twinsanity sea, waterfalls, sky).
		glm::vec2 uvScroll{0.0f, 0.0f};

		TextureHandle albedoTex{};
		TextureHandle normalTex{};
		TextureHandle metallicRoughnessTex{};
		TextureHandle occlusionTex{};
		TextureHandle emissiveTex{};

		MaterialTemplate templateDesc{.shaderVfsPath = "shaders://gltf_mesh.spv"};
	};
} // namespace aether
