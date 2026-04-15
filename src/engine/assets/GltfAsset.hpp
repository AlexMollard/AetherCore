#pragma once

#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "Mesh.hpp"

namespace aether::assets
{
	struct GltfImage
	{
		std::string name;
		std::string uri;
	};

	struct GltfTexture
	{
		std::string name;
		std::int32_t imageIndex = -1;
	};

	struct GltfMaterial
	{
		std::string name;
		glm::vec4 baseColorFactor{ 1.0f };
		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
		glm::vec3 emissiveFactor{ 0.0f };
		float alphaCutoff = 0.5f;
		bool doubleSided = false;
		bool alphaBlend = false;
		bool alphaMask = false;
		std::int32_t baseColorTexture = -1;
		std::int32_t metallicRoughnessTexture = -1;
		std::int32_t normalTexture = -1;
		std::int32_t occlusionTexture = -1;
		std::int32_t emissiveTexture = -1;
	};

	struct GltfSkin
	{
		std::string name;
		std::int32_t skeletonRoot = -1;
		std::vector<std::uint32_t> joints;
		std::vector<glm::mat4> inverseBindMatrices;
	};

	struct GltfNode
	{
		std::string name;
		std::int32_t parentIndex = -1;
		std::vector<std::uint32_t> children;
		std::int32_t meshIndex = -1;
		std::int32_t skinIndex = -1;
		glm::vec3 translation{ 0.0f };
		glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
		glm::vec3 scale{ 1.0f };
		glm::mat4 matrix{ 1.0f };
		bool hasMatrix = false;
	};

	struct GltfPrimitive
	{
		std::uint32_t nodeIndex = 0;
		std::int32_t materialIndex = -1;
		std::int32_t skinIndex = -1;
		std::vector<Mesh::Vertex> vertices;
		std::vector<std::uint32_t> indices;
	};

	enum class GltfAnimationPath
	{
		Translation,
		Rotation,
		Scale,
		Weights,
	};

	enum class GltfInterpolation
	{
		Linear,
		Step,
		CubicSpline,
	};

	struct GltfAnimationChannel
	{
		std::uint32_t nodeIndex = 0;
		GltfAnimationPath path = GltfAnimationPath::Translation;
		GltfInterpolation interpolation = GltfInterpolation::Linear;
		std::vector<float> times;
		std::vector<glm::vec4> values;
	};

	struct GltfAnimation
	{
		std::string name;
		std::vector<GltfAnimationChannel> channels;
	};

	struct GltfAsset
	{
		std::vector<GltfImage> images;
		std::vector<GltfTexture> textures;
		std::vector<GltfMaterial> materials;
		std::vector<GltfNode> nodes;
		std::vector<GltfSkin> skins;
		std::vector<GltfPrimitive> primitives;
		std::vector<GltfAnimation> animations;

		static GltfAsset LoadFromVfsPath(std::string_view path);
	};
} // namespace aether::assets
