#pragma once

#include <cstdint>
#include <glm/glm.hpp>

#include "material/MaterialTemplate.hpp"
#include "material/TextureHandle.hpp"

namespace aether
{
	// Pure authoring description of a surface. No GPU slot - that lives in the
	// MaterialRegistry. Texture refs are ref-counted TextureHandles (resolved to
	// raw bindless heap indices at pack time). Packed into a GpuMaterial by
	// PackMaterial().
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
		// When true, the shader multiplies base color by the mesh's vertex color
		// (used by primitive meshes that carry meaningful vertex colors).
		bool modulateVertexColor = false;

		// Default-constructed (invalid) handle = "optional map not set" -> the
		// shader skips the sample (GpuMaterial::kNoTexture at pack time).
		TextureHandle albedoTex{};
		TextureHandle normalTex{};
		TextureHandle metallicRoughnessTex{};
		TextureHandle occlusionTex{};
		TextureHandle emissiveTex{};

		// The pipeline this surface draws with. doubleSided/alphaBlend map into
		// templateDesc.cullMode/blendEnable at AssignMaterial time. Default is the
		// standard opaque gltf pipeline (today's BuildDefaultPipeline). Not read by
		// PackMaterial, so it does not affect content-addressed material dedup.
		MaterialTemplate templateDesc{.shaderVfsPath = "shaders://gltf_mesh.spv"};
	};
} // namespace aether
