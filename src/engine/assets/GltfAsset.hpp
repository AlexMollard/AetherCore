#pragma once

#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "mesh/Mesh.hpp"
#include "utils/Expected.hpp"

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
		glm::vec4 baseColorFactor{1.0f};
		float metallicFactor = 1.0f;
		float roughnessFactor = 1.0f;
		glm::vec3 emissiveFactor{0.0f};
		float alphaCutoff = 0.5f;
		bool doubleSided = false;
		bool alphaBlend = false;
		bool alphaMask = false;
		std::int32_t baseColorTexture = -1;
		std::int32_t metallicRoughnessTexture = -1;
		std::int32_t normalTexture = -1;
		std::int32_t occlusionTexture = -1;
		std::int32_t emissiveTexture = -1;

		// Texture paths from binary .material files (resolved at runtime).
		std::string albedoPath;
		std::string normalPath;
		std::string metallicRoughnessPath;
		std::string occlusionPath;
		std::string emissivePath;
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
		glm::vec3 translation{0.0f};
		glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 scale{1.0f};
		glm::mat4 matrix{1.0f};
		bool hasMatrix = false;
	};

	struct GltfPrimitive
	{
		std::uint32_t nodeIndex = 0;
		std::int32_t materialIndex = -1;
		std::int32_t skinIndex = -1;
		std::vector<Mesh::Vertex> vertices;
		std::vector<std::uint32_t> indices;

		// Bounding volume (from mesh header).
		float aabbMin[3] = {0, 0, 0};
		float aabbMax[3] = {0, 0, 0};
		float sphereCenter[3] = {0, 0, 0};
		float sphereRadius = 0.0f;
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

		[[nodiscard]] static Expected<GltfAsset> LoadFromVfsPath(std::string_view path);

		// Load from raw .mesh file data (already read from disk).
		// Useful after an async I/O operation.
		[[nodiscard]] static Expected<GltfAsset> LoadFromMemory(std::vector<std::byte> meshData, std::string_view debugPath);

		// Resolve the VFS path of the .mesh file for a given model path.
		// Returns an empty string if the path has no VFS mount.
		[[nodiscard]] static std::string ResolveMeshPath(std::string_view vfsPath);
	};
} // namespace aether::assets
