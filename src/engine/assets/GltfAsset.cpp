#include "assets/GltfAsset.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "FileSystem.hpp"

namespace aether::assets
{
	namespace
	{
		constexpr std::int32_t kInvalidIndex = -1;

			std::pair<std::string_view, std::string_view> SplitVfsPath(std::string_view vfsPath)
			{
				constexpr std::string_view kSeparator = "://";
				const std::size_t sep = vfsPath.find(kSeparator);
				if (sep == std::string_view::npos)
				{
					throw std::runtime_error("Invalid VFS path (missing ://): " + std::string(vfsPath));
				}
				return { vfsPath.substr(0, sep), vfsPath.substr(sep + kSeparator.size()) };
			}

			std::string ResolveRelativeVfsPath(std::string_view baseFilePath, std::string_view relativePath)
			{
				if (relativePath.starts_with("data:"))
				{
					return std::string(relativePath);
				}

				if (relativePath.find("://") != std::string_view::npos)
				{
					return std::string(relativePath);
				}

				auto [mount, baseRelative] = SplitVfsPath(baseFilePath);
				std::filesystem::path base = std::filesystem::path(std::string(baseRelative)).parent_path();
				std::filesystem::path resolved = (base / std::filesystem::path(std::string(relativePath))).lexically_normal();
				return std::string(mount) + "://" + resolved.generic_string();
			}

		std::int32_t ToIndex(const cgltf_node* value, const cgltf_data& data)
		{
			if (value == nullptr || data.nodes == nullptr)
			{
				return kInvalidIndex;
			}
			return static_cast<std::int32_t>(value - data.nodes);
		}

		std::int32_t ToIndex(const cgltf_mesh* value, const cgltf_data& data)
		{
			if (value == nullptr || data.meshes == nullptr)
			{
				return kInvalidIndex;
			}
			return static_cast<std::int32_t>(value - data.meshes);
		}

		std::int32_t ToIndex(const cgltf_skin* value, const cgltf_data& data)
		{
			if (value == nullptr || data.skins == nullptr)
			{
				return kInvalidIndex;
			}
			return static_cast<std::int32_t>(value - data.skins);
		}

		std::int32_t ToIndex(const cgltf_material* value, const cgltf_data& data)
		{
			if (value == nullptr || data.materials == nullptr)
			{
				return kInvalidIndex;
			}
			return static_cast<std::int32_t>(value - data.materials);
		}

		std::int32_t ToIndex(const cgltf_image* value, const cgltf_data& data)
		{
			if (value == nullptr || data.images == nullptr)
			{
				return kInvalidIndex;
			}
			return static_cast<std::int32_t>(value - data.images);
		}

		std::int32_t ToIndex(const cgltf_texture* value, const cgltf_data& data)
		{
			if (value == nullptr || data.textures == nullptr)
			{
				return kInvalidIndex;
			}
			return static_cast<std::int32_t>(value - data.textures);
		}

		std::string ToString(const char* text)
		{
			return text != nullptr ? std::string(text) : std::string();
		}

		GltfInterpolation ToInterpolation(cgltf_interpolation_type interpolation)
		{
			switch (interpolation)
			{
			case cgltf_interpolation_type_step:
				return GltfInterpolation::Step;
			case cgltf_interpolation_type_cubic_spline:
				return GltfInterpolation::CubicSpline;
			case cgltf_interpolation_type_linear:
			default:
				return GltfInterpolation::Linear;
			}
		}

		GltfAnimationPath ToPath(cgltf_animation_path_type path)
		{
			switch (path)
			{
			case cgltf_animation_path_type_rotation:
				return GltfAnimationPath::Rotation;
			case cgltf_animation_path_type_scale:
				return GltfAnimationPath::Scale;
			case cgltf_animation_path_type_weights:
				return GltfAnimationPath::Weights;
			case cgltf_animation_path_type_translation:
			default:
				return GltfAnimationPath::Translation;
			}
		}

		const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type)
		{
			for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
			{
				const cgltf_attribute& attr = primitive.attributes[i];
				if (attr.type == type && attr.data != nullptr)
				{
					return attr.data;
				}
			}
			return nullptr;
		}

		void ReadTransform(const cgltf_node& source, GltfNode& destination)
		{
			if (source.has_translation)
			{
				destination.translation = glm::vec3(
					static_cast<float>(source.translation[0]),
					static_cast<float>(source.translation[1]),
					static_cast<float>(source.translation[2]));
			}
			if (source.has_rotation)
			{
				destination.rotation = glm::quat(
					static_cast<float>(source.rotation[3]),
					static_cast<float>(source.rotation[0]),
					static_cast<float>(source.rotation[1]),
					static_cast<float>(source.rotation[2]));
			}
			if (source.has_scale)
			{
				destination.scale = glm::vec3(
					static_cast<float>(source.scale[0]),
					static_cast<float>(source.scale[1]),
					static_cast<float>(source.scale[2]));
			}
			if (source.has_matrix)
			{
				destination.hasMatrix = true;
				std::memcpy(&destination.matrix[0][0], source.matrix, sizeof(source.matrix));
			}
		}

		std::vector<std::uint32_t> BuildIdentityIndices(std::size_t count)
		{
			std::vector<std::uint32_t> out(count);
			for (std::size_t i = 0; i < count; ++i)
			{
				out[i] = static_cast<std::uint32_t>(i);
			}
			return out;
		}
	}

	GltfAsset GltfAsset::LoadFromVfsPath(std::string_view path)
	{
		const std::string vfsPath(path);
		if (!io::FileSystem::Exists(vfsPath))
		{
			throw std::runtime_error("glTF file not found in VFS: " + vfsPath);
		}

		const std::vector<std::byte> sourceBytes = io::FileSystem::ReadFile(vfsPath);
		if (sourceBytes.empty())
		{
			throw std::runtime_error("glTF file is empty: " + vfsPath);
		}

		cgltf_options options{};
		cgltf_data* data = nullptr;
		const cgltf_result parseResult = cgltf_parse(
			&options,
			sourceBytes.data(),
			sourceBytes.size(),
			&data);
		if (parseResult != cgltf_result_success || data == nullptr)
		{
			throw std::runtime_error("Failed to parse glTF file: " + vfsPath);
		}

		std::vector<std::vector<std::byte>> loadedBuffers;
		loadedBuffers.reserve(data->buffers_count);
		for (cgltf_size i = 0; i < data->buffers_count; ++i)
		{
			cgltf_buffer& buffer = data->buffers[i];
			if (buffer.data != nullptr || buffer.uri == nullptr)
			{
				continue;
			}

			const std::string resolvedBufferPath = ResolveRelativeVfsPath(vfsPath, buffer.uri);
			std::vector<std::byte> bufferBytes = io::FileSystem::ReadFile(resolvedBufferPath);
			if (bufferBytes.empty())
			{
				cgltf_free(data);
				throw std::runtime_error("Failed to load glTF buffer via VFS: " + resolvedBufferPath);
			}

			buffer.size = static_cast<cgltf_size>(bufferBytes.size());
			buffer.data = bufferBytes.data();
			loadedBuffers.push_back(std::move(bufferBytes));
			data->buffers[i].data = loadedBuffers.back().data();
		}

		const cgltf_result validateResult = cgltf_validate(data);
		if (validateResult != cgltf_result_success)
		{
			cgltf_free(data);
			throw std::runtime_error("glTF validation failed: " + vfsPath);
		}

		GltfAsset asset;

		asset.images.reserve(data->images_count);
		for (cgltf_size i = 0; i < data->images_count; ++i)
		{
			const cgltf_image& image = data->images[i];
			const std::string imageUri = ToString(image.uri);
			asset.images.push_back({
				.name = ToString(image.name),
				.uri = imageUri.empty() ? std::string() : ResolveRelativeVfsPath(vfsPath, imageUri),
			});
		}

		asset.textures.reserve(data->textures_count);
		for (cgltf_size i = 0; i < data->textures_count; ++i)
		{
			const cgltf_texture& texture = data->textures[i];
			asset.textures.push_back({
				.name = ToString(texture.name),
				.imageIndex = ToIndex(texture.image, *data),
			});
		}

		asset.materials.reserve(data->materials_count);
		for (cgltf_size i = 0; i < data->materials_count; ++i)
		{
			const cgltf_material& material = data->materials[i];
			GltfMaterial out;
			out.name = ToString(material.name);
			out.baseColorFactor = glm::vec4(
				static_cast<float>(material.pbr_metallic_roughness.base_color_factor[0]),
				static_cast<float>(material.pbr_metallic_roughness.base_color_factor[1]),
				static_cast<float>(material.pbr_metallic_roughness.base_color_factor[2]),
				static_cast<float>(material.pbr_metallic_roughness.base_color_factor[3]));
			out.metallicFactor = static_cast<float>(material.pbr_metallic_roughness.metallic_factor);
			out.roughnessFactor = static_cast<float>(material.pbr_metallic_roughness.roughness_factor);
			out.emissiveFactor = glm::vec3(
				static_cast<float>(material.emissive_factor[0]),
				static_cast<float>(material.emissive_factor[1]),
				static_cast<float>(material.emissive_factor[2]));
			out.alphaCutoff = static_cast<float>(material.alpha_cutoff);
			out.doubleSided = material.double_sided;
			out.alphaBlend = material.alpha_mode == cgltf_alpha_mode_blend;
			out.alphaMask = material.alpha_mode == cgltf_alpha_mode_mask;
			out.baseColorTexture = ToIndex(material.pbr_metallic_roughness.base_color_texture.texture, *data);
			out.metallicRoughnessTexture = ToIndex(material.pbr_metallic_roughness.metallic_roughness_texture.texture, *data);
			out.normalTexture = ToIndex(material.normal_texture.texture, *data);
			out.occlusionTexture = ToIndex(material.occlusion_texture.texture, *data);
			out.emissiveTexture = ToIndex(material.emissive_texture.texture, *data);
			asset.materials.push_back(std::move(out));
		}

		asset.nodes.resize(data->nodes_count);
		for (cgltf_size i = 0; i < data->nodes_count; ++i)
		{
			const cgltf_node& node = data->nodes[i];
			GltfNode& out = asset.nodes[i];
			out.name = ToString(node.name);
			out.meshIndex = ToIndex(node.mesh, *data);
			out.skinIndex = ToIndex(node.skin, *data);
			ReadTransform(node, out);
			out.children.reserve(node.children_count);
			for (cgltf_size c = 0; c < node.children_count; ++c)
			{
				const std::int32_t childIndex = ToIndex(node.children[c], *data);
				if (childIndex >= 0)
				{
					out.children.push_back(static_cast<std::uint32_t>(childIndex));
				}
			}
		}

		for (cgltf_size i = 0; i < data->nodes_count; ++i)
		{
			const cgltf_node& node = data->nodes[i];
			if (node.parent != nullptr)
			{
				asset.nodes[i].parentIndex = ToIndex(node.parent, *data);
			}
		}

		asset.skins.reserve(data->skins_count);
		for (cgltf_size i = 0; i < data->skins_count; ++i)
		{
			const cgltf_skin& skin = data->skins[i];
			GltfSkin out;
			out.name = ToString(skin.name);
			out.skeletonRoot = ToIndex(skin.skeleton, *data);
			out.joints.reserve(skin.joints_count);
			for (cgltf_size joint = 0; joint < skin.joints_count; ++joint)
			{
				out.joints.push_back(static_cast<std::uint32_t>(ToIndex(skin.joints[joint], *data)));
			}

			if (skin.inverse_bind_matrices != nullptr)
			{
				out.inverseBindMatrices.resize(skin.inverse_bind_matrices->count);
				std::array<float, 16> matrix{};
				for (cgltf_size m = 0; m < skin.inverse_bind_matrices->count; ++m)
				{
					if (!cgltf_accessor_read_float(skin.inverse_bind_matrices, m, matrix.data(), matrix.size()))
					{
						continue;
					}
					std::memcpy(&out.inverseBindMatrices[m][0][0], matrix.data(), sizeof(float) * matrix.size());
				}
			}
			asset.skins.push_back(std::move(out));
		}

		for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex)
		{
			const cgltf_node& node = data->nodes[nodeIndex];
			if (node.mesh == nullptr)
			{
				continue;
			}

			for (cgltf_size primIndex = 0; primIndex < node.mesh->primitives_count; ++primIndex)
			{
				const cgltf_primitive& primitive = node.mesh->primitives[primIndex];
				if (primitive.type != cgltf_primitive_type_triangles)
				{
					continue;
				}

				const cgltf_accessor* position = FindAttribute(primitive, cgltf_attribute_type_position);
				if (position == nullptr)
				{
					continue;
				}

				const cgltf_accessor* normal = FindAttribute(primitive, cgltf_attribute_type_normal);
				const cgltf_accessor* tangent = FindAttribute(primitive, cgltf_attribute_type_tangent);
				const cgltf_accessor* texcoord0 = FindAttribute(primitive, cgltf_attribute_type_texcoord);
				const cgltf_accessor* color0 = FindAttribute(primitive, cgltf_attribute_type_color);
				const cgltf_accessor* joints0 = FindAttribute(primitive, cgltf_attribute_type_joints);
				const cgltf_accessor* weights0 = FindAttribute(primitive, cgltf_attribute_type_weights);

				GltfPrimitive out;
				out.nodeIndex = static_cast<std::uint32_t>(nodeIndex);
				out.materialIndex = ToIndex(primitive.material, *data);
				out.skinIndex = ToIndex(node.skin, *data);
				out.vertices.resize(position->count);

				std::array<float, 4> floatValues{};
				std::array<cgltf_uint, 4> uintValues{};
				for (cgltf_size v = 0; v < position->count; ++v)
				{
					Mesh::Vertex vertex{};

					cgltf_accessor_read_float(position, v, floatValues.data(), 3);
					vertex.position = glm::vec3(floatValues[0], floatValues[1], floatValues[2]);

					if (normal != nullptr)
					{
						cgltf_accessor_read_float(normal, v, floatValues.data(), 3);
						vertex.normal = glm::vec3(floatValues[0], floatValues[1], floatValues[2]);
					}
					if (tangent != nullptr)
					{
						cgltf_accessor_read_float(tangent, v, floatValues.data(), 4);
						vertex.tangent = glm::vec4(floatValues[0], floatValues[1], floatValues[2], floatValues[3]);
					}
					else
					{
						vertex.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
					}
					if (texcoord0 != nullptr)
					{
						cgltf_accessor_read_float(texcoord0, v, floatValues.data(), 2);
						vertex.uv = glm::vec2(floatValues[0], floatValues[1]);
					}
					if (color0 != nullptr)
					{
						cgltf_accessor_read_float(color0, v, floatValues.data(), 4);
						vertex.color = glm::vec3(floatValues[0], floatValues[1], floatValues[2]);
					}
					else
					{
						vertex.color = glm::vec3(1.0f);
					}
					if (joints0 != nullptr)
					{
						cgltf_accessor_read_uint(joints0, v, uintValues.data(), 4);
						vertex.jointIndices = glm::uvec4(
							static_cast<std::uint32_t>(uintValues[0]),
							static_cast<std::uint32_t>(uintValues[1]),
							static_cast<std::uint32_t>(uintValues[2]),
							static_cast<std::uint32_t>(uintValues[3]));
					}
					if (weights0 != nullptr)
					{
						cgltf_accessor_read_float(weights0, v, floatValues.data(), 4);
						vertex.jointWeights = glm::vec4(floatValues[0], floatValues[1], floatValues[2], floatValues[3]);
					}
					else
					{
						vertex.jointWeights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
					}

					out.vertices[v] = vertex;
				}

				if (primitive.indices != nullptr)
				{
					out.indices.resize(primitive.indices->count);
					for (cgltf_size i = 0; i < primitive.indices->count; ++i)
					{
						out.indices[i] = static_cast<std::uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
					}
				}
				else
				{
					out.indices = BuildIdentityIndices(out.vertices.size());
				}

				// Some glTFs (including Fox) omit NORMAL attributes.
				// In that case, generate smooth vertex normals from indexed triangles.
				if (normal == nullptr)
				{
					for (auto& vtx : out.vertices)
					{
						vtx.normal = glm::vec3(0.0f);
					}

					for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3)
					{
						const std::uint32_t ia = out.indices[i + 0];
						const std::uint32_t ib = out.indices[i + 1];
						const std::uint32_t ic = out.indices[i + 2];

						if (ia >= out.vertices.size() || ib >= out.vertices.size() || ic >= out.vertices.size())
						{
							continue;
						}

						const glm::vec3& a = out.vertices[ia].position;
						const glm::vec3& b = out.vertices[ib].position;
						const glm::vec3& c = out.vertices[ic].position;
						const glm::vec3 faceN = glm::cross(b - a, c - a);

						if (glm::dot(faceN, faceN) > 1e-16f)
						{
							out.vertices[ia].normal += faceN;
							out.vertices[ib].normal += faceN;
							out.vertices[ic].normal += faceN;
						}
					}

					for (auto& vtx : out.vertices)
					{
						const float len2 = glm::dot(vtx.normal, vtx.normal);
						vtx.normal = (len2 > 1e-16f)
							? glm::normalize(vtx.normal)
							: glm::vec3(0.0f, 1.0f, 0.0f);
					}
				}

				asset.primitives.push_back(std::move(out));
			}
		}

		asset.animations.reserve(data->animations_count);
		for (cgltf_size animIndex = 0; animIndex < data->animations_count; ++animIndex)
		{
			const cgltf_animation& animation = data->animations[animIndex];
			GltfAnimation outAnim;
			outAnim.name = ToString(animation.name);
			outAnim.channels.reserve(animation.channels_count);

			for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex)
			{
				const cgltf_animation_channel& channel = animation.channels[channelIndex];
				if (channel.sampler == nullptr || channel.target_node == nullptr)
				{
					continue;
				}

				const cgltf_accessor* input = channel.sampler->input;
				const cgltf_accessor* output = channel.sampler->output;
				if (input == nullptr || output == nullptr)
				{
					continue;
				}

				GltfAnimationChannel outChannel;
				const std::int32_t targetNodeIndex = ToIndex(channel.target_node, *data);
				if (targetNodeIndex < 0)
				{
					continue;
				}

				outChannel.nodeIndex = static_cast<std::uint32_t>(targetNodeIndex);
				outChannel.path = ToPath(channel.target_path);
				outChannel.interpolation = ToInterpolation(channel.sampler->interpolation);

				std::array<float, 4> value{};
				const bool isRotation = outChannel.path == GltfAnimationPath::Rotation;
				const cgltf_size componentCount = isRotation ? 4 : 3;
				const cgltf_size keyCount = std::min(input->count, output->count);
				outChannel.times.resize(keyCount);
				outChannel.values.resize(keyCount);
				for (cgltf_size i = 0; i < keyCount; ++i)
				{
					cgltf_accessor_read_float(input, i, value.data(), 1);
					outChannel.times[i] = value[0];

					cgltf_accessor_read_float(output, i, value.data(), componentCount);
					outChannel.values[i] = glm::vec4(
						value[0],
						value[1],
						value[2],
						isRotation ? value[3] : 0.0f);
				}

				outAnim.channels.push_back(std::move(outChannel));
			}

			asset.animations.push_back(std::move(outAnim));
		}

		cgltf_free(data);
		return asset;
	}
}
