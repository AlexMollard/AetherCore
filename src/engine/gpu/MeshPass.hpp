#pragma once

#include <cmath>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include "material/MaterialAsset.hpp"
#include "material/GpuMaterial.hpp"

namespace aether
{
	struct MeshRenderInstance
	{
		glm::mat4 world{1.0f};
		glm::vec4 color{1.0f};
		std::uint32_t mesh = 0;
		std::uint32_t customShaderSlot = UINT32_MAX;
		std::uint32_t proceduralKind = 0;
		glm::vec4 effectTint{1.0f};
		glm::vec4 effectParams{1.0f, 1.0f, 1.0f, 0.0f};
		MaterialAsset material{};
		std::array<std::uint32_t, 5> textures{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
		std::vector<glm::mat4> skinMatrices;
		bool castShadows = true;
	};

	struct Render3DFrameData
	{
		std::vector<MeshRenderInstance> meshes;
	};
} // namespace aether

namespace aether::gpu
{
	struct MeshGpuInstance
	{
		glm::mat4 world{1.0f};
		glm::vec4 normal0{}, normal1{}, normal2{};
		glm::vec4 color{1.0f};
		glm::vec4 emissiveRoughness{};
		glm::vec4 factors{}; // metallic, occlusion, alpha cutoff, reserved
		glm::vec4 uvScroll{};
		glm::uvec4 metadata{}; // source draw, palette offset, palette count, material flags
	};

	static_assert(sizeof(MeshGpuInstance) == 192);
	static_assert(offsetof(MeshGpuInstance, color) == 112);
	static_assert(offsetof(MeshGpuInstance, metadata) == 176);
	inline constexpr std::uint32_t kMeshMirrored = 1u << 31;

	[[nodiscard]] inline bool ValidMeshSkinInfluences(const glm::uvec4& joints, const glm::vec4& weights, std::uint32_t jointCount, bool requireSkin = true)
	{
		float sum = 0;
		for (unsigned i = 0; i < 4; ++i)
		{
			if (!std::isfinite(weights[i]) || weights[i] < 0)
			{
				return false;
			}
			sum += weights[i];
		}
		if (!requireSkin)
		{
			// Cooked static streams may retain UINT32_MAX indices even with unit weights.
			return sum == 0.0f || std::abs(sum - 1.0f) <= 0.001f;
		}
		for (unsigned i = 0; i < 4; ++i)
		{
			// Match the native shader's guarded zero-weight/sentinel influences.
			if (weights[i] > 0 && joints[i] >= jointCount)
			{
				return false;
			}
		}
		return std::abs(sum - 1.0f) <= 0.001f;
	}

	// Opaque draws retain authored order; transparent draws follow them, farthest first.
	[[nodiscard]] inline std::vector<std::uint32_t> MeshDrawOrder(const Render3DFrameData& frame, const glm::mat4& view)
	{
		std::vector<std::uint32_t> order;
		for (std::uint32_t i = 0; i < frame.meshes.size(); ++i)
		{
			if (!frame.meshes[i].material.alphaBlend)
			{
				order.push_back(i);
			}
		}
		const auto start = order.size();
		for (std::uint32_t i = 0; i < frame.meshes.size(); ++i)
		{
			if (frame.meshes[i].material.alphaBlend)
			{
				order.push_back(i);
			}
		}
		std::stable_sort(order.begin() + start, order.end(), [&](auto a, auto b) { return (view * frame.meshes[a].world[3]).z < (view * frame.meshes[b].world[3]).z; });
		return order;
	}

	[[nodiscard]] inline std::expected<std::vector<MeshGpuInstance>, std::string> PlanMeshPass(const Render3DFrameData& frame, std::size_t limit = 4096)
	{
		if (frame.meshes.size() > limit)
		{
			return std::unexpected("Mesh instance frame budget exceeded");
		}
		std::vector<MeshGpuInstance> instances;
		instances.reserve(frame.meshes.size());
		std::uint32_t paletteOffset = 0;
		for (const auto& mesh: frame.meshes)
		{
			const auto& material = mesh.material;
			const auto baseColor = mesh.color * material.baseColorFactor;
			if (mesh.skinMatrices.size() > 256 || paletteOffset + mesh.skinMatrices.size() > 65536)
			{
				return std::unexpected("Mesh skin palette budget exceeded");
			}
			for (const auto& matrix: mesh.skinMatrices)
			{
				for (int c = 0; c < 4; ++c)
				{
					for (int r = 0; r < 4; ++r)
					{
						if (!std::isfinite(matrix[c][r]))
						{
							return std::unexpected("Nonfinite skin matrix");
						}
					}
				}
				if (matrix[0].w != 0 || matrix[1].w != 0 || matrix[2].w != 0 || matrix[3].w != 1)
				{
					return std::unexpected("Skin matrix must be affine");
				}
			}
			for (int c = 0; c < 4; ++c)
			{
				if (!std::isfinite(material.baseColorFactor[c]) || !std::isfinite(baseColor[c]))
				{
					return std::unexpected("Nonfinite material color");
				}
			}
			for (int c = 0; c < 3; ++c)
			{
				if (!std::isfinite(material.emissiveFactor[c]))
				{
					return std::unexpected("Nonfinite material emissive");
				}
			}
			if (!std::isfinite(material.metallicFactor) || !std::isfinite(material.roughnessFactor) || !std::isfinite(material.occlusionStrength) || !std::isfinite(material.alphaCutoff) || !std::isfinite(material.uvScroll.x)
			        || !std::isfinite(material.uvScroll.y) || (material.additiveBlend && !material.alphaBlend))
			{
				return std::unexpected("Invalid mesh material factors or blend mode");
			}
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(mesh.world[column][row]))
					{
						return std::unexpected("Nonfinite mesh transform");
					}
				}
				if (!std::isfinite(mesh.color[column]))
				{
					return std::unexpected("Nonfinite mesh color");
				}
			}
			if (mesh.world[0].w != 0 || mesh.world[1].w != 0 || mesh.world[2].w != 0 || mesh.world[3].w != 1)
			{
				return std::unexpected("Mesh world transform must be affine");
			}
			const auto determinant = glm::determinant(glm::mat3(mesh.world));
			if (!std::isfinite(determinant) || std::abs(determinant) < 1e-8f)
			{
				return std::unexpected("Singular mesh transform");
			}
			const auto normal = glm::inverseTranspose(glm::mat3(mesh.world));
			for (int column = 0; column < 3; ++column)
			{
				for (int row = 0; row < 3; ++row)
				{
					if (!std::isfinite(normal[column][row]))
					{
						return std::unexpected("Nonfinite mesh normal transform");
					}
				}
			}
			const std::uint32_t flags = (material.modulateVertexColor ? GpuMaterial::kModulateVertexColor : 0u) | (material.alphaMask ? GpuMaterial::kAlphaMask : 0u) | (determinant < 0 ? kMeshMirrored : 0u)
			                            | (material.alphaBlend ? GpuMaterial::kAlphaBlend : 0u) | (material.additiveBlend ? GpuMaterial::kAdditiveBlend : 0u) | (material.doubleSided ? GpuMaterial::kDoubleSided : 0u)
			                            | (!material.receiveShadows ? GpuMaterial::kNoReceiveShadows : 0u) | (material.foliage ? GpuMaterial::kFoliage : 0u) | (material.bakedLighting ? GpuMaterial::kBakedLighting : 0u)
			                            | (material.sky ? GpuMaterial::kSky : 0u) | (material.lightShaft ? GpuMaterial::kLightShaft : 0u) | (material.objectLit ? GpuMaterial::kObjectLit : 0u) | (material.gem ? GpuMaterial::kGem : 0u)
			                            | (material.sheen ? GpuMaterial::kSheen : 0u) | (material.water ? GpuMaterial::kWater : 0u) | (material.character ? GpuMaterial::kCharacter : 0u);
			instances.push_back({mesh.world,
			        glm::vec4(normal[0], 0),
			        glm::vec4(normal[1], 0),
			        glm::vec4(normal[2], 0),
			        baseColor,
			        glm::vec4(material.emissiveFactor, material.roughnessFactor),
			        {material.metallicFactor, material.occlusionStrength, material.alphaCutoff, 0},
			        glm::vec4(material.uvScroll, 0, 0),
			        {static_cast<std::uint32_t>(instances.size()), paletteOffset, static_cast<std::uint32_t>(mesh.skinMatrices.size()), flags}});
			paletteOffset += static_cast<std::uint32_t>(mesh.skinMatrices.size());
		}
		return instances;
	}
} // namespace aether::gpu
