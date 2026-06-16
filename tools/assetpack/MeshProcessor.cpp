#include "MeshProcessor.hpp"

#include "PipelineUtils.hpp"

#include <BinaryFormats.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <cgltf.h>

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace MeshProcessor
{
	namespace
	{
		// -------------------------------------------------------------------------
		// Index helpers
		// -------------------------------------------------------------------------

		int32_t ToIndex(const cgltf_node* value, const cgltf_data& data)
		{
			if (!value || !data.nodes)
			{
				return -1;
			}
			return static_cast<int32_t>(value - data.nodes);
		}

		int32_t ToIndex(const cgltf_material* value, const cgltf_data& data)
		{
			if (!value || !data.materials)
			{
				return -1;
			}
			return static_cast<int32_t>(value - data.materials);
		}

		std::string SafeStr(const char* s)
		{
			return s ? s : "";
		}

		// Strip common mixamo/rig prefixes from a bone name so that
		// skeletons and animations from different exports use consistent
		// bare names (e.g. "mixamorig:Hips" and "mixamorig_Hips" both → "Hips").
		std::string StripBonePrefix(const std::string& name)
		{
			static constexpr const char* kPrefixes[] = {"mixamorig:", "mixamorig_", "Armature_"};
			for (const auto prefix: kPrefixes)
			{
				const std::size_t plen = std::strlen(prefix);
				if (name.size() > plen && name.compare(0, plen, prefix) == 0)
				{
					return name.substr(plen);
				}
			}
			return name;
		}

		// -------------------------------------------------------------------------
		// Attribute lookup
		// -------------------------------------------------------------------------

		const cgltf_accessor* FindAttr(const cgltf_primitive& prim, cgltf_attribute_type type, int idx = 0)
		{
			for (cgltf_size i = 0; i < prim.attributes_count; ++i)
			{
				const auto& a = prim.attributes[i];
				if (a.type == type && a.index == idx && a.data)
				{
					return a.data;
				}
			}
			return nullptr;
		}

		// -------------------------------------------------------------------------
		// Skin resolution
		// -------------------------------------------------------------------------

		const cgltf_skin* FindSkinForPrimitive(const cgltf_node& node, const cgltf_primitive& prim, const cgltf_data& data)
		{
			if (node.skin)
			{
				return node.skin;
			}

			for (const cgltf_node* anc = node.parent; anc != nullptr; anc = anc->parent)
			{
				if (anc->skin)
				{
					return anc->skin;
				}
			}

			std::array<cgltf_uint, 4> jointIdx{};
			const cgltf_accessor* jointsAcc = FindAttr(prim, cgltf_attribute_type_joints, 0);
			if (!jointsAcc)
			{
				return nullptr;
			}
			cgltf_uint maxIdx = 0;
			for (cgltf_size v = 0; v < jointsAcc->count; ++v)
			{
				cgltf_accessor_read_uint(jointsAcc, v, jointIdx.data(), 4);
				for (uint32_t j = 0; j < 4; ++j)
				{
					if (jointIdx[j] > maxIdx)
					{
						maxIdx = jointIdx[j];
					}
				}
			}
			for (cgltf_size si = 0; si < data.skins_count; ++si)
			{
				const cgltf_skin& s = data.skins[si];
				if (static_cast<cgltf_size>(maxIdx) < s.joints_count)
				{
					return &s;
				}
			}
			return nullptr;
		}

		// -------------------------------------------------------------------------
		// Normal generation (angle-weighted)
		// -------------------------------------------------------------------------

		void GenerateNormals(std::vector<DiskMeshVertex>& verts, const std::vector<uint32_t>& idx)
		{
			for (auto& v: verts)
			{
				v.normal[0] = v.normal[1] = v.normal[2] = 0.f;
			}

			for (std::size_t i = 0; i + 2 < idx.size(); i += 3)
			{
				const uint32_t ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
				if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size())
				{
					continue;
				}

				Vec3 a{verts[ia].position[0], verts[ia].position[1], verts[ia].position[2]};
				Vec3 b{verts[ib].position[0], verts[ib].position[1], verts[ib].position[2]};
				Vec3 c{verts[ic].position[0], verts[ic].position[1], verts[ic].position[2]};

				Vec3 edge1 = b - a;
				Vec3 edge2 = c - a;
				Vec3 n = glm::cross(edge1, edge2);

				float eLen = glm::length(edge1);
				float fLen = glm::length(edge2);
				float angleA = (eLen > 1e-8f && fLen > 1e-8f) ? std::acos(glm::clamp(glm::dot(edge1, edge2) / (eLen * fLen), -1.f, 1.f)) : 1.f;

				edge1 = a - b;
				edge2 = c - b;
				float gLen = glm::length(edge1);
				float hLen = glm::length(edge2);
				float angleB = (gLen > 1e-8f && hLen > 1e-8f) ? std::acos(glm::clamp(glm::dot(edge1, edge2) / (gLen * hLen), -1.f, 1.f)) : 1.f;

				edge1 = a - c;
				edge2 = b - c;
				float iLen = glm::length(edge1);
				float jLen = glm::length(edge2);
				float angleC = (iLen > 1e-8f && jLen > 1e-8f) ? std::acos(glm::clamp(glm::dot(edge1, edge2) / (iLen * jLen), -1.f, 1.f)) : 1.f;

				verts[ia].normal[0] += n.x * angleA;
				verts[ia].normal[1] += n.y * angleA;
				verts[ia].normal[2] += n.z * angleA;
				verts[ib].normal[0] += n.x * angleB;
				verts[ib].normal[1] += n.y * angleB;
				verts[ib].normal[2] += n.z * angleB;
				verts[ic].normal[0] += n.x * angleC;
				verts[ic].normal[1] += n.y * angleC;
				verts[ic].normal[2] += n.z * angleC;
			}

			for (auto& v: verts)
			{
				Vec3 n{v.normal[0], v.normal[1], v.normal[2]};
				n = glm::normalize(n);
				v.normal[0] = n.x;
				v.normal[1] = n.y;
				v.normal[2] = n.z;
			}
		}

		// -------------------------------------------------------------------------
		// Color packing: float[4] -> RGBA8 uint32
		// -------------------------------------------------------------------------

		uint32_t PackColorRGBA8(const float rgba[4])
		{
			uint32_t r = static_cast<uint32_t>(std::clamp(rgba[0], 0.f, 1.f) * 255.f);
			uint32_t g = static_cast<uint32_t>(std::clamp(rgba[1], 0.f, 1.f) * 255.f);
			uint32_t b = static_cast<uint32_t>(std::clamp(rgba[2], 0.f, 1.f) * 255.f);
			uint32_t a = static_cast<uint32_t>(std::clamp(rgba[3], 0.f, 1.f) * 255.f);
			return (r << 0) | (g << 8) | (b << 16) | (a << 24);
		}

		// -------------------------------------------------------------------------
		// 4x4 column-major matrix helpers
		// -------------------------------------------------------------------------

		void Mat4MulVec3(const float m[16], Vec3 in, Vec3& out)
		{
			out.x = m[0] * in.x + m[4] * in.y + m[8] * in.z + m[12];
			out.y = m[1] * in.x + m[5] * in.y + m[9] * in.z + m[13];
			out.z = m[2] * in.x + m[6] * in.y + m[10] * in.z + m[14];
		}

		void Mat3InverseTransposeMulVec3(const float m[16], Vec3 in, Vec3& out)
		{
			const float a = m[0], b = m[4], c = m[8];
			const float d = m[1], e = m[5], f = m[9];
			const float g = m[2], h = m[6], i = m[10];
			const float A = e * i - f * h;
			const float B = f * g - d * i;
			const float C = d * h - e * g;
			const float D = c * h - b * i;
			const float E = a * i - c * g;
			const float F = b * g - a * h;
			const float G = b * f - c * e;
			const float H = c * d - a * f;
			const float I = a * e - b * d;
			const float det = a * A + b * B + c * C;
			const float invDet = 1.f / det;
			out.x = (A * in.x + D * in.y + G * in.z) * invDet;
			out.y = (B * in.x + E * in.y + H * in.z) * invDet;
			out.z = (C * in.x + F * in.y + I * in.z) * invDet;
		}

		// -------------------------------------------------------------------------
		// Bounding volume computation
		// -------------------------------------------------------------------------

		struct Bounds
		{
			float aabbMin[3] = {0, 0, 0};
			float aabbMax[3] = {0, 0, 0};
			float sphereCenter[3] = {0, 0, 0};
			float sphereRadius = 0.0f;
		};

		Bounds ComputeBounds(const std::vector<DiskMeshVertex>& verts)
		{
			Bounds bounds;
			if (verts.empty())
			{
				return bounds;
			}

			Vec3 p0{verts[0].position[0], verts[0].position[1], verts[0].position[2]};
			bounds.aabbMin[0] = bounds.aabbMax[0] = p0.x;
			bounds.aabbMin[1] = bounds.aabbMax[1] = p0.y;
			bounds.aabbMin[2] = bounds.aabbMax[2] = p0.z;

			for (const auto& v: verts)
			{
				bounds.aabbMin[0] = std::min(bounds.aabbMin[0], v.position[0]);
				bounds.aabbMin[1] = std::min(bounds.aabbMin[1], v.position[1]);
				bounds.aabbMin[2] = std::min(bounds.aabbMin[2], v.position[2]);
				bounds.aabbMax[0] = std::max(bounds.aabbMax[0], v.position[0]);
				bounds.aabbMax[1] = std::max(bounds.aabbMax[1], v.position[1]);
				bounds.aabbMax[2] = std::max(bounds.aabbMax[2], v.position[2]);
			}

			bounds.sphereCenter[0] = (bounds.aabbMin[0] + bounds.aabbMax[0]) * 0.5f;
			bounds.sphereCenter[1] = (bounds.aabbMin[1] + bounds.aabbMax[1]) * 0.5f;
			bounds.sphereCenter[2] = (bounds.aabbMin[2] + bounds.aabbMax[2]) * 0.5f;

			for (const auto& v: verts)
			{
				const float dx = v.position[0] - bounds.sphereCenter[0];
				const float dy = v.position[1] - bounds.sphereCenter[1];
				const float dz = v.position[2] - bounds.sphereCenter[2];
				const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
				bounds.sphereRadius = std::max(bounds.sphereRadius, dist);
			}

			return bounds;
		}

		// -------------------------------------------------------------------------
		// Bone sorting and remap
		// -------------------------------------------------------------------------

		struct BoneInfo
		{
			std::string name;
			int32_t originalIndex;
			int32_t parentIndex; // original
			std::array<float, 16> ibm;
		};

		uint64_t ComputeSkeletonHash(const std::vector<BoneInfo>& sortedBones, const std::vector<uint32_t>& remapTable)
		{
			XXH3_state_t state;
			XXH3_64bits_reset(&state);

			for (const auto& bone: sortedBones)
			{
				// name bytes (no null terminator)
				XXH3_64bits_update(&state, bone.name.data(), bone.name.size());
				// remapped parent index
				const int32_t remappedParent = (bone.parentIndex >= 0) ? static_cast<int32_t>(remapTable[static_cast<std::size_t>(bone.parentIndex)]) : -1;
				XXH3_64bits_update(&state, &remappedParent, sizeof(remappedParent));
				// ibm[16] column-major
				XXH3_64bits_update(&state, bone.ibm.data(), sizeof(float) * 16);
			}

			return XXH3_64bits_digest(&state);
		}

		// -------------------------------------------------------------------------
		// Collect bones from all skins
		// -------------------------------------------------------------------------

		std::vector<BoneInfo> CollectBones(const cgltf_data& data)
		{
			std::vector<BoneInfo> bones;
			std::vector<bool> seen(data.nodes_count, false);

			for (cgltf_size si = 0; si < data.skins_count; ++si)
			{
				const cgltf_skin& skin = data.skins[si];
				for (cgltf_size ji = 0; ji < skin.joints_count; ++ji)
				{
					const int32_t nodeIdx = ToIndex(skin.joints[ji], data);
					if (nodeIdx < 0 || seen[static_cast<std::size_t>(nodeIdx)])
					{
						continue;
					}
					seen[static_cast<std::size_t>(nodeIdx)] = true;

					BoneInfo info;
					info.name = StripBonePrefix(SafeStr(data.nodes[static_cast<std::size_t>(nodeIdx)].name));
					info.originalIndex = nodeIdx;
					info.parentIndex = (data.nodes[static_cast<std::size_t>(nodeIdx)].parent) ? ToIndex(data.nodes[static_cast<std::size_t>(nodeIdx)].parent, data) : -1;

					// Read inverse bind matrix
					if (skin.inverse_bind_matrices && ji < skin.inverse_bind_matrices->count)
					{
						cgltf_accessor_read_float(skin.inverse_bind_matrices, ji, info.ibm.data(), 16);
					}
					else
					{
						info.ibm = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
					}

					bones.push_back(std::move(info));
				}
			}

			return bones;
		}

		// -------------------------------------------------------------------------
		// Material name extraction
		// -------------------------------------------------------------------------

		std::string GetMaterialName(const cgltf_data& data, int32_t materialIndex)
		{
			if (materialIndex < 0 || static_cast<std::size_t>(materialIndex) >= data.materials_count)
			{
				return "default";
			}
			const std::string name = SafeStr(data.materials[static_cast<std::size_t>(materialIndex)].name);
			return name.empty() ? "material_" + std::to_string(materialIndex) : name;
		}

		// -------------------------------------------------------------------------
		// Skeleton processing
		// -------------------------------------------------------------------------

		struct SkeletonResult
		{
			std::vector<BoneInfo> bones;
			std::vector<uint32_t> remapTable;
			std::vector<std::byte> skelData;
			std::string skelHashStr;
			uint64_t skelHash = 0;
			bool valid = false;
		};

		SkeletonResult ProcessSkeleton(cgltf_data* data, const std::string& sourcePath)
		{
			SkeletonResult out;
			std::vector<BoneInfo> bones = CollectBones(*data);
			if (bones.empty())
			{
				return out;
			}

			out.bones = std::move(bones);

			// Build remap table
			out.remapTable.assign(data->nodes_count, static_cast<uint32_t>(-1));

			// Sort bones by name for deterministic hash
			std::stable_sort(out.bones.begin(), out.bones.end(), [](const BoneInfo& a, const BoneInfo& b) { return a.name < b.name; });

			for (std::size_t i = 0; i < out.bones.size(); ++i)
			{
				out.remapTable[static_cast<std::size_t>(out.bones[i].originalIndex)] = i;
			}

			out.skelHash = ComputeSkeletonHash(out.bones, out.remapTable);
			out.skelHashStr = std::to_string(out.skelHash);

			// Write .skel file
			const std::string skelName = Stem(sourcePath);
			SkelHeaderDisk hdr;
			hdr.boneCount = static_cast<uint32_t>(out.bones.size());
			hdr.nameLen = static_cast<uint16_t>(skelName.size());
			hdr.skeletonHash = out.skelHash;

			Append(out.skelData, hdr);
			AppendStringData(out.skelData, skelName);

			for (const auto& bone: out.bones)
			{
				BoneEntryHeaderDisk boneHdr;
				boneHdr.nameLen = static_cast<uint16_t>(bone.name.size());
				Append(out.skelData, boneHdr);
				AppendStringData(out.skelData, bone.name);

				const int32_t remappedParent = (bone.parentIndex >= 0) ? static_cast<int32_t>(out.remapTable[static_cast<std::size_t>(bone.parentIndex)]) : -1;
				Append(out.skelData, remappedParent);
				AppendBytes(out.skelData, bone.ibm.data(), sizeof(float) * 16);
			}

			out.valid = true;
			return out;
		}

		// -------------------------------------------------------------------------
		// Material path collection
		// -------------------------------------------------------------------------

		std::vector<std::string> CollectMaterialPaths(cgltf_data* data, const std::filesystem::path& sourceDir)
		{
			std::vector<std::string> paths;
			std::vector<bool> seen(data->materials_count, false);
			for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
			{
				const cgltf_node& node = data->nodes[ni];
				if (!node.mesh)
				{
					continue;
				}
				for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
				{
					const int32_t matIdx = ToIndex(node.mesh->primitives[pi].material, *data);
					if (matIdx >= 0 && !seen[static_cast<std::size_t>(matIdx)])
					{
						seen[static_cast<std::size_t>(matIdx)] = true;
						const std::string matName = GetMaterialName(*data, matIdx);
						const std::string matDirPath = (sourceDir / "materials" / (matName + ".material") / "properties.toml").string();
						if (std::filesystem::exists(matDirPath))
						{
							paths.push_back("materials/" + matName + ".material");
						}
					}
				}
			}
			return paths;
		}

		// -------------------------------------------------------------------------
		// Auto-generate binary .material data from glTF material data
		// -------------------------------------------------------------------------

		struct GltfMatResult
		{
			std::string vfsPath;
			std::vector<std::byte> data;
		};

		GltfMatResult GenerateGlTFMaterialData(const cgltf_material& mat, const cgltf_data& data, const std::string& gltfVfsPath)
		{
			const std::string matName = GetMaterialName(data, static_cast<int32_t>(&mat - data.materials));

			// Material VFS path: <gltf-dir>/materials/<name>.material
			const fs::path gltfRel = fs::path(gltfVfsPath).parent_path();
			const std::string matVfsPath = (gltfRel / "materials" / (matName + ".material")).generic_string();

			MaterialHeaderDisk hdr;
			if (mat.has_pbr_metallic_roughness)
			{
				std::memcpy(hdr.baseColorFactor, mat.pbr_metallic_roughness.base_color_factor, sizeof(hdr.baseColorFactor));
				hdr.metallicFactor = mat.pbr_metallic_roughness.metallic_factor;
				hdr.roughnessFactor = mat.pbr_metallic_roughness.roughness_factor;
			}
			std::memcpy(hdr.emissiveFactor, mat.emissive_factor, sizeof(hdr.emissiveFactor));
			hdr.alphaCutoff = mat.alpha_cutoff;
			hdr.doubleSided = mat.double_sided ? 1 : 0;
			hdr.alphaBlend = (mat.alpha_mode == cgltf_alpha_mode_blend) ? 1 : 0;
			hdr.alphaMask = (mat.alpha_mode == cgltf_alpha_mode_mask) ? 1 : 0;

			struct TexSlot
			{
				TextureTypeDisk type;
				cgltf_texture_view view;
			};

			std::vector<TexSlot> slots;
			if (mat.has_pbr_metallic_roughness)
			{
				if (mat.pbr_metallic_roughness.base_color_texture.texture)
				{
					slots.push_back({TextureTypeDisk::BaseColor, mat.pbr_metallic_roughness.base_color_texture});
				}
				if (mat.pbr_metallic_roughness.metallic_roughness_texture.texture)
				{
					slots.push_back({TextureTypeDisk::MetallicRoughness, mat.pbr_metallic_roughness.metallic_roughness_texture});
				}
			}
			if (mat.normal_texture.texture)
			{
				slots.push_back({TextureTypeDisk::Normal, mat.normal_texture});
			}
			if (mat.occlusion_texture.texture)
			{
				slots.push_back({TextureTypeDisk::Occlusion, mat.occlusion_texture});
			}
			if (mat.emissive_texture.texture)
			{
				slots.push_back({TextureTypeDisk::Emissive, mat.emissive_texture});
			}

			// Resolve image URI to mount-relative VFS path
			auto imageVfsPath = [&](const cgltf_image* img) -> std::string
			{
				if (!img || !img->uri)
				{
					return {};
				}
				// Image URI is relative to the glTF file
				fs::path imgPath = fs::path(gltfVfsPath).parent_path() / img->uri;
				return imgPath.lexically_normal().generic_string();
			};

			std::vector<std::byte> matData;
			auto append = [&](const void* p, std::size_t n)
			{
				const auto bytes = reinterpret_cast<const std::byte*>(p);
				matData.insert(matData.end(), bytes, bytes + n);
			};

			hdr.texturePathCount = 0;
			// First pass: count valid textures
			for (const auto& slot: slots)
			{
				if (slot.view.texture && slot.view.texture->image && slot.view.texture->image->uri)
				{
					++hdr.texturePathCount;
				}
			}

			append(&hdr, sizeof(hdr));

			// Second pass: write textures
			for (const auto& slot: slots)
			{
				if (!slot.view.texture || !slot.view.texture->image || !slot.view.texture->image->uri)
				{
					continue;
				}
				const std::string texPath = imageVfsPath(slot.view.texture->image);
				if (texPath.empty())
				{
					continue;
				}
				const uint8_t typeByte = static_cast<uint8_t>(slot.type);
				const uint16_t pathLen = static_cast<uint16_t>(texPath.size());
				append(&typeByte, sizeof(typeByte));
				append(&pathLen, sizeof(pathLen));
				append(texPath.data(), texPath.size());
			}

			return {matVfsPath, std::move(matData)};
		}

		// -------------------------------------------------------------------------
		// Mesh primitive extraction
		// -------------------------------------------------------------------------

		struct SubMeshInfo
		{
			uint32_t firstIndex;
			uint32_t indexCount;
			int32_t materialIndex; // glTF material index, -1 = none
		};

		struct MeshExtractResult
		{
			std::vector<DiskMeshVertex> verts;
			std::vector<uint32_t> indices;
			Bounds bounds;
			std::string skinRefPath;
			std::vector<SubMeshInfo> subMeshes;
		};

		MeshExtractResult ExtractMeshes(cgltf_data* data, const std::vector<uint32_t>& remapTable, const std::string& virtualPath, const std::string& sourcePath)
		{
			MeshExtractResult out;

			auto WarnEightInfluences = [&](const cgltf_primitive& prim, const std::string& primDesc)
			{
				if (FindAttr(prim, cgltf_attribute_type_joints, 1))
				{
					std::cerr << "  MeshProcessor: WARNING - " << primDesc << " has JOINTS_1/WEIGHTS_1 (8+ influences). "
					          << "Only the first 4 influences are stored.\n";
				}
			};

			uint32_t vertexOffset = 0;

			for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
			{
				const cgltf_node& node = data->nodes[ni];
				if (!node.mesh)
				{
					continue;
				}

				float worldMat[16];
				cgltf_node_transform_world(&node, worldMat);

				for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
				{
					const cgltf_primitive& prim = node.mesh->primitives[pi];
					if (prim.type != cgltf_primitive_type_triangles)
					{
						continue;
					}
					const cgltf_accessor* posAcc = FindAttr(prim, cgltf_attribute_type_position, 0);
					if (!posAcc)
					{
						continue;
					}

					const uint32_t vertCount = static_cast<uint32_t>(posAcc->count);
					const cgltf_accessor* normAcc = FindAttr(prim, cgltf_attribute_type_normal, 0);
					const cgltf_accessor* tanAcc = FindAttr(prim, cgltf_attribute_type_tangent, 0);
					const cgltf_accessor* uvAcc = FindAttr(prim, cgltf_attribute_type_texcoord, 0);
					const cgltf_accessor* uv2Acc = FindAttr(prim, cgltf_attribute_type_texcoord, 1);
					const cgltf_accessor* colorAcc = FindAttr(prim, cgltf_attribute_type_color, 0);
					const cgltf_accessor* jointsAcc = FindAttr(prim, cgltf_attribute_type_joints, 0);
					const cgltf_accessor* weightsAcc = FindAttr(prim, cgltf_attribute_type_weights, 0);

					const bool isSkinned = (jointsAcc != nullptr);

					const cgltf_skin* resolvedSkin = isSkinned ? FindSkinForPrimitive(node, prim, *data) : nullptr;
					if (isSkinned && !resolvedSkin)
					{
						std::cerr << "  MeshProcessor: WARNING - skinned primitive on '" << SafeStr(node.name) << "' has no resolvable skin; joint indices will remain sentinel.\n";
					}

					std::vector<DiskMeshVertex> verts(vertCount);
					for (uint32_t v = 0; v < vertCount; ++v)
					{
						std::memset(&verts[v]._pad, 0, sizeof(verts[v]._pad));
					}
					std::array<cgltf_uint, 4> jointIdx{};

					for (uint32_t v = 0; v < vertCount; ++v)
					{
						DiskMeshVertex& dst = verts[v];

						if (isSkinned)
						{
							cgltf_accessor_read_float(posAcc, v, dst.position, 3);
						}
						else
						{
							Vec3 worldPos;
							Vec3 posIn;
							cgltf_accessor_read_float(posAcc, v, &posIn.x, 3);
							Mat4MulVec3(worldMat, posIn, worldPos);
							dst.position[0] = worldPos.x;
							dst.position[1] = worldPos.y;
							dst.position[2] = worldPos.z;
						}

						if (normAcc)
						{
							cgltf_accessor_read_float(normAcc, v, dst.normal, 3);
							if (!isSkinned)
							{
								Vec3 normIn{dst.normal[0], dst.normal[1], dst.normal[2]};
								Vec3 normOut;
								Mat3InverseTransposeMulVec3(worldMat, normIn, normOut);
								dst.normal[0] = normOut.x;
								dst.normal[1] = normOut.y;
								dst.normal[2] = normOut.z;
							}
						}

						if (tanAcc)
						{
							cgltf_accessor_read_float(tanAcc, v, dst.tangent, 4);
						}
						else
						{
							dst.tangent[0] = 1.f;
							dst.tangent[1] = 0.f;
							dst.tangent[2] = 0.f;
							dst.tangent[3] = 1.f;
						}

						if (uvAcc)
						{
							cgltf_accessor_read_float(uvAcc, v, dst.uv, 2);
						}

						if (uv2Acc)
						{
							cgltf_accessor_read_float(uv2Acc, v, dst.uv2, 2);
						}

						if (colorAcc)
						{
							float color[4];
							cgltf_accessor_read_float(colorAcc, v, color, 4);
							dst.color = PackColorRGBA8(color);
						}
						else
						{
							dst.color = 0xFFFFFFFF;
						}

						dst.jointIndices[0] = 0xFFFFFFFF;
						dst.jointIndices[1] = 0xFFFFFFFF;
						dst.jointIndices[2] = 0xFFFFFFFF;
						dst.jointIndices[3] = 0xFFFFFFFF;
						dst.jointWeights[0] = 1.f;
						dst.jointWeights[1] = 0.f;
						dst.jointWeights[2] = 0.f;
						dst.jointWeights[3] = 0.f;

						if (jointsAcc)
						{
							cgltf_accessor_read_uint(jointsAcc, v, jointIdx.data(), 4);

							for (uint32_t j = 0; j < 4; ++j)
							{
								if (!resolvedSkin)
								{
									continue;
								}
								if (jointIdx[j] >= resolvedSkin->joints_count)
								{
									continue;
								}
								const int32_t nodeIdx = ToIndex(resolvedSkin->joints[jointIdx[j]], *data);
								if (nodeIdx < 0 || static_cast<std::size_t>(nodeIdx) >= remapTable.size())
								{
									continue;
								}
								const uint32_t boneIdx = remapTable[static_cast<std::size_t>(nodeIdx)];
								if (boneIdx == static_cast<uint32_t>(-1))
								{
									continue;
								}
								dst.jointIndices[j] = boneIdx;
							}
						}

						if (weightsAcc)
						{
							cgltf_accessor_read_float(weightsAcc, v, dst.jointWeights, 4);

							float wsum = dst.jointWeights[0] + dst.jointWeights[1] + dst.jointWeights[2] + dst.jointWeights[3];
							if (wsum > 1e-6f)
							{
								const float inv = 1.f / wsum;
								dst.jointWeights[0] *= inv;
								dst.jointWeights[1] *= inv;
								dst.jointWeights[2] *= inv;
								dst.jointWeights[3] *= inv;
							}
						}
					}

					WarnEightInfluences(prim, SafeStr(node.name) + " primitive " + std::to_string(pi));

					// Build index buffer
					std::vector<uint32_t> indices;
					if (prim.indices)
					{
						indices.resize(prim.indices->count);
						for (cgltf_size k = 0; k < prim.indices->count; ++k)
						{
							indices[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k)) + vertexOffset;
						}
					}
					else
					{
						indices.resize(vertCount);
						for (uint32_t k = 0; k < vertCount; ++k)
						{
							indices[k] = k + vertexOffset;
						}
					}

					if (!normAcc)
					{
						GenerateNormals(verts, indices);
					}

					out.subMeshes.push_back({
					        .firstIndex = static_cast<uint32_t>(out.indices.size()),
					        .indexCount = static_cast<uint32_t>(indices.size()),
					        .materialIndex = ToIndex(prim.material, *data),
					});

					out.verts.insert(out.verts.end(), verts.begin(), verts.end());
					out.indices.insert(out.indices.end(), indices.begin(), indices.end());
					vertexOffset += vertCount;
				}
			}

			// Compute bounds
			if (!out.verts.empty())
			{
				Vec3 p0{out.verts[0].position[0], out.verts[0].position[1], out.verts[0].position[2]};
				out.bounds.aabbMin[0] = out.bounds.aabbMax[0] = p0.x;
				out.bounds.aabbMin[1] = out.bounds.aabbMax[1] = p0.y;
				out.bounds.aabbMin[2] = out.bounds.aabbMax[2] = p0.z;

				for (const auto& v: out.verts)
				{
					out.bounds.aabbMin[0] = std::min(out.bounds.aabbMin[0], v.position[0]);
					out.bounds.aabbMin[1] = std::min(out.bounds.aabbMin[1], v.position[1]);
					out.bounds.aabbMin[2] = std::min(out.bounds.aabbMin[2], v.position[2]);
					out.bounds.aabbMax[0] = std::max(out.bounds.aabbMax[0], v.position[0]);
					out.bounds.aabbMax[1] = std::max(out.bounds.aabbMax[1], v.position[1]);
					out.bounds.aabbMax[2] = std::max(out.bounds.aabbMax[2], v.position[2]);
				}

				// Ritter's bounding sphere
				{
					const auto& verts = out.verts;
					const std::size_t n = verts.size();
					if (n > 0)
					{
						Vec3 P{verts[0].position[0], verts[0].position[1], verts[0].position[2]};
						std::size_t Q = 0;
						float maxDistSq = 0.f;
						for (std::size_t r = 1; r < n; ++r)
						{
							Vec3 vr{verts[r].position[0], verts[r].position[1], verts[r].position[2]};
							float d = glm::dot(vr - P, vr - P);
							if (d > maxDistSq)
							{
								maxDistSq = d;
								Q = r;
							}
						}
						Vec3 Qp{verts[Q].position[0], verts[Q].position[1], verts[Q].position[2]};
						std::size_t R = 0;
						maxDistSq = 0.f;
						for (std::size_t r = 0; r < n; ++r)
						{
							Vec3 vr{verts[r].position[0], verts[r].position[1], verts[r].position[2]};
							float d = glm::dot(vr - Qp, vr - Qp);
							if (d > maxDistSq)
							{
								maxDistSq = d;
								R = r;
							}
						}
						Vec3 Rp{verts[R].position[0], verts[R].position[1], verts[R].position[2]};
						out.bounds.sphereCenter[0] = (Qp.x + Rp.x) * 0.5f;
						out.bounds.sphereCenter[1] = (Qp.y + Rp.y) * 0.5f;
						out.bounds.sphereCenter[2] = (Qp.z + Rp.z) * 0.5f;
						out.bounds.sphereRadius = glm::length(Rp - Vec3{out.bounds.sphereCenter[0], out.bounds.sphereCenter[1], out.bounds.sphereCenter[2]});
						for (std::size_t r = 0; r < n; ++r)
						{
							Vec3 vr{verts[r].position[0], verts[r].position[1], verts[r].position[2]};
							Vec3 currentCenter{out.bounds.sphereCenter[0], out.bounds.sphereCenter[1], out.bounds.sphereCenter[2]};
							float d = glm::length(vr - currentCenter);
							if (d > out.bounds.sphereRadius)
							{
								const float half = (d - out.bounds.sphereRadius) * 0.5f;
								out.bounds.sphereRadius += half;
								Vec3 dir = (vr - currentCenter) / d;
								out.bounds.sphereCenter[0] += half * dir.x;
								out.bounds.sphereCenter[1] += half * dir.y;
								out.bounds.sphereCenter[2] += half * dir.z;
							}
						}
					}
				}
			}

			// Skin ref path
			const std::string skinRefDir = std::filesystem::path(virtualPath).parent_path().generic_string();
			const bool hasBones = !remapTable.empty();
			out.skinRefPath = hasBones ? (skinRefDir.empty() ? (Stem(sourcePath) + ".skel") : (skinRefDir + "/" + Stem(sourcePath) + ".skel")) : "";

			return out;
		}

		// -------------------------------------------------------------------------
		// Animation processing
		// -------------------------------------------------------------------------

		struct AnimResult
		{
			std::vector<std::pair<std::string, std::vector<std::byte>>> files;
			std::vector<std::string> paths; // for .animset
		};

		AnimResult ProcessAnimations(cgltf_data* data, const std::vector<uint32_t>& remapTable, const std::string& sourcePath, bool nameBased = false)
		{
			AnimResult out;
			if (data->animations_count == 0)
			{
				return out;
			}

			// For animation-only glTFs: build virtual remap from animated nodes
			std::vector<uint32_t> virtualRemap;
			std::vector<std::string> virtualBoneNames;
			if (nameBased && remapTable.empty())
			{
				std::vector<int32_t> animatedNodes;
				for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
				{
					for (cgltf_size ci = 0; ci < data->animations[ai].channels_count; ++ci)
					{
						const auto& ch = data->animations[ai].channels[ci];
						if (!ch.sampler || !ch.target_node || !ch.sampler->input || !ch.sampler->output)
						{
							continue;
						}
						int32_t idx = ToIndex(ch.target_node, *data);
						if (idx >= 0 && std::find(animatedNodes.begin(), animatedNodes.end(), idx) == animatedNodes.end())
						{
							animatedNodes.push_back(idx);
						}
					}
				}

				if (!animatedNodes.empty())
				{
					std::sort(animatedNodes.begin(), animatedNodes.end());
					virtualRemap.assign(data->nodes_count, static_cast<uint32_t>(-1));
					for (std::size_t i = 0; i < animatedNodes.size(); ++i)
					{
						virtualRemap[animatedNodes[i]] = static_cast<uint32_t>(i);
						virtualBoneNames.push_back(StripBonePrefix(SafeStr(data->nodes[animatedNodes[i]].name)));
					}
				}
			}

			const bool hasBoneNames = nameBased && !virtualRemap.empty();
			const auto& activeRemap = virtualRemap.empty() ? remapTable : virtualRemap;

			for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
			{
				const cgltf_animation& anim = data->animations[ai];
				const std::string animName = SafeStr(anim.name);
				const std::string fileName = Stem(sourcePath) + "_" + (animName.empty() ? std::to_string(ai) : animName) + ".anim";

				std::vector<std::byte> animData;

				uint32_t validChannels = 0;
				for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
				{
					const auto& ch = anim.channels[ci];
					if (!ch.sampler || !ch.target_node || !ch.sampler->input || !ch.sampler->output)
					{
						continue;
					}
					const int32_t targetNode = ToIndex(ch.target_node, *data);
					if (targetNode < 0 || static_cast<std::size_t>(targetNode) >= activeRemap.size())
					{
						continue;
					}
					if (activeRemap[static_cast<std::size_t>(targetNode)] == static_cast<uint32_t>(-1))
					{
						continue;
					}
					++validChannels;
				}

				AnimHeaderDisk animHdr;
				animHdr.channelCount = validChannels;
				animHdr.nameLen = static_cast<uint16_t>(animName.size());
				animHdr.flags = hasBoneNames ? ANIM_FLAG_HAS_BONE_NAMES : 0;
				Append(animData, animHdr);
				AppendStringData(animData, animName);

				for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
				{
					const cgltf_animation_channel& ch = anim.channels[ci];
					if (!ch.sampler || !ch.target_node || !ch.sampler->input || !ch.sampler->output)
					{
						continue;
					}

					const int32_t targetNode = ToIndex(ch.target_node, *data);
					if (targetNode < 0 || static_cast<std::size_t>(targetNode) >= activeRemap.size())
					{
						continue;
					}

					const uint32_t remappedNode = activeRemap[static_cast<std::size_t>(targetNode)];
					if (remappedNode == static_cast<uint32_t>(-1))
					{
						continue;
					}

					AnimPathDisk path;
					switch (ch.target_path)
					{
						case cgltf_animation_path_type_rotation:
							path = AnimPathDisk::Rotation;
							break;
						case cgltf_animation_path_type_scale:
							path = AnimPathDisk::Scale;
							break;
						case cgltf_animation_path_type_weights:
							path = AnimPathDisk::Weights;
							break;
						default:
							path = AnimPathDisk::Translation;
							break;
					}

					AnimInterpDisk interp;
					switch (ch.sampler->interpolation)
					{
						case cgltf_interpolation_type_step:
							interp = AnimInterpDisk::Step;
							break;
						case cgltf_interpolation_type_cubic_spline:
							interp = AnimInterpDisk::CubicSpline;
							break;
						default:
							interp = AnimInterpDisk::Linear;
							break;
					}

					const cgltf_accessor* inputAcc = ch.sampler->input;
					const cgltf_accessor* outputAcc = ch.sampler->output;
					const uint32_t keyCount = static_cast<uint32_t>(std::min(inputAcc->count, outputAcc->count));

					ChannelHeaderDisk chHdr;
					chHdr.nodeIndex = remappedNode;
					chHdr.path = static_cast<uint8_t>(path);
					chHdr.interp = static_cast<uint8_t>(interp);
					chHdr.keyCount = keyCount;
					Append(animData, chHdr);

					if (hasBoneNames)
					{
						const std::string& boneName = virtualBoneNames[remappedNode];
						if (std::addressof(animData) != nullptr && boneName.empty())
						{
							std::cerr << "  WARNING: empty bone name for remappedNode=" << remappedNode << " in " << fileName << "\n";
						}
						uint16_t nameLen = static_cast<uint16_t>(boneName.size());
						Append(animData, nameLen);
						AppendStringData(animData, boneName);
					}

					std::array<float, 4> val{};
					for (uint32_t k = 0; k < keyCount; ++k)
					{
						cgltf_accessor_read_float(inputAcc, k, val.data(), 1);
						Append(animData, val[0]);
					}

					const bool isRotation = (path == AnimPathDisk::Rotation);
					const uint32_t compCount = isRotation ? 4 : 3;
					for (uint32_t k = 0; k < keyCount; ++k)
					{
						val[3] = 0.f;
						cgltf_accessor_read_float(outputAcc, k, val.data(), compCount);
						AppendBytes(animData, val.data(), sizeof(float) * 4);
					}
				}

				out.files.emplace_back(fileName, std::move(animData));
				out.paths.push_back("animations/" + fileName);
			}

			return out;
		}

		void WriteAnimSet(std::vector<std::byte>& out, const AnimResult& anim, uint64_t skelHash)
		{
			AnimSetHeaderDisk setHdr;
			setHdr.animCount = static_cast<uint32_t>(anim.paths.size());
			setHdr.skeletonHash = skelHash;

			Append(out, setHdr);
			for (const auto& animPath: anim.paths)
			{
				AppendStr(out, animPath);
			}
		}

	} // namespace

	// -------------------------------------------------------------------------

	ProcessedResult Process(std::span<const std::byte> gltfData, const std::filesystem::path& sourcePath, const std::string& virtualPath, const std::filesystem::path& sourceDir)
	{
		cgltf_options options{};
		cgltf_data* data = nullptr;

		if (cgltf_parse(&options, gltfData.data(), gltfData.size(), &data) != cgltf_result_success)
		{
			std::cerr << "  MeshProcessor: cgltf_parse failed for " << sourcePath << "\n";
			return {};
		}

		const std::string srcPathStr = sourcePath.string();
		if (cgltf_load_buffers(&options, data, srcPathStr.c_str()) != cgltf_result_success)
		{
			std::cerr << "  MeshProcessor: failed to load buffers for " << sourcePath << "\n";
			cgltf_free(data);
			return {};
		}

		if (cgltf_validate(data) != cgltf_result_success)
		{
			std::cerr << "  MeshProcessor: cgltf_validate failed for " << sourcePath << "\n";
			cgltf_free(data);
			return {};
		}

		ProcessedResult result;

		// ── Skeleton ────────────────────────────────────────────────────────────
		auto skel = ProcessSkeleton(data, sourcePath.string());
		if (skel.valid)
		{
			result.skelData = std::move(skel.skelData);
			result.skeletonHash = skel.skelHashStr;
		}

		// ── Material paths ──────────────────────────────────────────────────────
		auto materialPaths = CollectMaterialPaths(data, sourceDir);

		// Find glTF materials not covered by existing .material files and auto-generate them
		{
			std::vector<bool> hasMat(data->materials_count, false);
			for (const auto& mp: materialPaths)
			{
				const std::string matName = fs::path(mp).stem().string();
				for (cgltf_size mi = 0; mi < data->materials_count; ++mi)
				{
					if (GetMaterialName(*data, static_cast<int32_t>(mi)) == matName)
					{
						hasMat[mi] = true;
						break;
					}
				}
			}
			for (cgltf_size mi = 0; mi < data->materials_count; ++mi)
			{
				if (hasMat[mi])
				{
					continue;
				}
				auto gen = GenerateGlTFMaterialData(data->materials[mi], *data, virtualPath);
				if (!gen.data.empty())
				{
					materialPaths.push_back(gen.vfsPath);
					result.materialFiles.emplace_back(gen.vfsPath, std::move(gen.data));
					std::cout << "  + auto-generated .material for '" << data->materials[mi].name << "'\n";
				}
			}
		}

		// ── Mesh extraction ─────────────────────────────────────────────────────
		auto mesh = ExtractMeshes(data, skel.remapTable, virtualPath, sourcePath.string());

		if (!mesh.verts.empty())
		{
			uint32_t maxIdx = 0;
			for (const auto& idx: mesh.indices)
			{
				if (idx > maxIdx)
				{
					maxIdx = idx;
				}
			}

			// Build glTF material index → materialPaths index mapping
			std::vector<int> matToPathIdx(data->materials_count, -1);
			for (cgltf_size mi = 0; mi < data->materials_count; ++mi)
			{
				const std::string name = GetMaterialName(*data, static_cast<int32_t>(mi));
				for (std::size_t pi = 0; pi < materialPaths.size(); ++pi)
				{
					if (fs::path(materialPaths[pi]).stem().string() == name)
					{
						matToPathIdx[mi] = static_cast<int>(pi);
						break;
					}
				}
			}

			MeshHeaderDisk hdr;
			hdr.vertexCount = static_cast<uint32_t>(mesh.verts.size());
			hdr.indexCount = static_cast<uint32_t>(mesh.indices.size());
			hdr.skinRefPathLen = static_cast<uint32_t>(mesh.skinRefPath.size());
			hdr.materialCount = static_cast<uint32_t>(materialPaths.size());
			hdr.subMeshCount = static_cast<uint32_t>(mesh.subMeshes.size());
			hdr.indexType = (maxIdx > 0xFFFF) ? 1 : 0;
			std::memcpy(hdr.aabbMin, mesh.bounds.aabbMin, sizeof(mesh.bounds.aabbMin));
			std::memcpy(hdr.aabbMax, mesh.bounds.aabbMax, sizeof(mesh.bounds.aabbMax));
			std::memcpy(hdr.sphereCenter, mesh.bounds.sphereCenter, sizeof(mesh.bounds.sphereCenter));
			hdr.sphereRadius = mesh.bounds.sphereRadius;

			Append(result.meshData, hdr);
			AppendBytes(result.meshData, mesh.verts.data(), mesh.verts.size() * sizeof(DiskMeshVertex));

			if (maxIdx > 0xFFFF)
			{
				AppendBytes(result.meshData, mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));
			}
			else
			{
				std::vector<uint16_t> idx16(mesh.indices.size());
				for (std::size_t i = 0; i < mesh.indices.size(); ++i)
				{
					idx16[i] = static_cast<uint16_t>(mesh.indices[i]);
				}
				AppendBytes(result.meshData, idx16.data(), idx16.size() * sizeof(uint16_t));
			}

			// Write submesh headers (v3+)
			for (const auto& sm: mesh.subMeshes)
			{
				SubMeshHeaderDisk smHdr;
				smHdr.firstIndex = sm.firstIndex;
				smHdr.indexCount = sm.indexCount;
				if (sm.materialIndex >= 0 && static_cast<std::size_t>(sm.materialIndex) < matToPathIdx.size() && matToPathIdx[sm.materialIndex] >= 0)
				{
					smHdr.materialIndex = static_cast<uint32_t>(matToPathIdx[sm.materialIndex]);
				}
				else
				{
					smHdr.materialIndex = 0;
				}
				Append(result.meshData, smHdr);
			}

			AppendStringData(result.meshData, mesh.skinRefPath);
			for (const auto& matPath: materialPaths)
			{
				AppendStr(result.meshData, matPath);
			}
		}

		// ── Animations ──────────────────────────────────────────────────────────
		const bool nameBased = !skel.valid && data->animations_count > 0;
		auto anims = ProcessAnimations(data, skel.remapTable, sourcePath.string(), nameBased);
		if (!anims.files.empty())
		{
			result.animFiles = std::move(anims.files);
			WriteAnimSet(result.animsetData, anims, skel.skelHash);
		}

		cgltf_free(data);
		return result;
	}

} // namespace MeshProcessor
