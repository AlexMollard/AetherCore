#include "MeshProcessor.hpp"

#include <BinaryFormats.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#include <xxhash.h>

namespace MeshProcessor
{
    namespace
    {
        // -------------------------------------------------------------------------
        // Write helpers
        // -------------------------------------------------------------------------

        template <typename T>
        void Append(std::vector<std::byte>& buf, const T& value)
        {
            const auto* p = reinterpret_cast<const std::byte*>(&value);
            buf.insert(buf.end(), p, p + sizeof(T));
        }

        void AppendBytes(std::vector<std::byte>& buf, const void* src, std::size_t n)
        {
            const auto* p = reinterpret_cast<const std::byte*>(src);
            buf.insert(buf.end(), p, p + n);
        }

        void AppendStr(std::vector<std::byte>& buf, const std::string& s)
        {
            assert(s.size() <= std::numeric_limits<uint16_t>::max() && "string too long for uint16_t length prefix");
            const uint16_t len = static_cast<uint16_t>(s.size());
            AppendBytes(buf, &len, sizeof(len));
            AppendBytes(buf, s.data(), s.size());
        }

        void AppendStringData(std::vector<std::byte>& buf, const std::string& s)
        {
            AppendBytes(buf, s.data(), s.size());
        }

        // -------------------------------------------------------------------------
        // Index helpers
        // -------------------------------------------------------------------------

        int32_t ToIndex(const cgltf_node* value, const cgltf_data& data)
        {
            if (!value || !data.nodes) return -1;
            return static_cast<int32_t>(value - data.nodes);
        }
        int32_t ToIndex(const cgltf_material* value, const cgltf_data& data)
        {
            if (!value || !data.materials) return -1;
            return static_cast<int32_t>(value - data.materials);
        }

        std::string SafeStr(const char* s) { return s ? s : ""; }

        std::string Stem(const std::filesystem::path& p)
        {
            return p.stem().string();
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
                    return a.data;
            }
            return nullptr;
        }

        // -------------------------------------------------------------------------
        // Normal generation
        // -------------------------------------------------------------------------

        struct TempVertex
        {
            float position[3];
            float normal[3];
            float tangent[4];
            float uv[2];
            float uv2[2];
            uint32_t color; // RGBA8 packed
            uint32_t jointIndices[4];
            float jointWeights[4];
        };

        void GenerateNormals(std::vector<TempVertex>& verts, const std::vector<uint32_t>& idx)
        {
            for (auto& v : verts)
                v.normal[0] = v.normal[1] = v.normal[2] = 0.f;

            for (std::size_t i = 0; i + 2 < idx.size(); i += 3)
            {
                const uint32_t ia = idx[i], ib = idx[i + 1], ic = idx[i + 2];
                if (ia >= verts.size() || ib >= verts.size() || ic >= verts.size()) continue;

                const float* a = verts[ia].position;
                const float* b = verts[ib].position;
                const float* c = verts[ic].position;

                const float ex = b[0]-a[0], ey = b[1]-a[1], ez = b[2]-a[2];
                const float fx = c[0]-a[0], fy = c[1]-a[1], fz = c[2]-a[2];
                const float nx = ey*fz - ez*fy;
                const float ny = ez*fx - ex*fz;
                const float nz = ex*fy - ey*fx;

                const float gx = a[0]-b[0], gy = a[1]-b[1], gz = a[2]-b[2];
                const float hx = c[0]-b[0], hy = c[1]-b[1], hz = c[2]-b[2];
                const float ix = a[0]-c[0], iy = a[1]-c[1], iz = a[2]-c[2];
                const float jx = b[0]-c[0], jy = b[1]-c[1], jz = b[2]-c[2];

                const float eLen = std::sqrt(ex*ex + ey*ey + ez*ez);
                const float fLen = std::sqrt(fx*fx + fy*fy + fz*fz);
                const float gLen = std::sqrt(gx*gx + gy*gy + gz*gz);
                const float hLen = std::sqrt(hx*hx + hy*hy + hz*hz);
                const float iLen = std::sqrt(ix*ix + iy*iy + iz*iz);
                const float jLen = std::sqrt(jx*jx + jy*jy + jz*jz);

                const float angleA = (eLen > 1e-8f && fLen > 1e-8f)
                    ? std::acos(std::clamp((ex*fx + ey*fy + ez*fz) / (eLen * fLen), -1.f, 1.f)) : 1.f;
                const float angleB = (gLen > 1e-8f && hLen > 1e-8f)
                    ? std::acos(std::clamp((gx*hx + gy*hy + gz*hz) / (gLen * hLen), -1.f, 1.f)) : 1.f;
                const float angleC = (iLen > 1e-8f && jLen > 1e-8f)
                    ? std::acos(std::clamp((ix*jx + iy*jy + iz*jz) / (iLen * jLen), -1.f, 1.f)) : 1.f;

                verts[ia].normal[0] += nx * angleA;
                verts[ia].normal[1] += ny * angleA;
                verts[ia].normal[2] += nz * angleA;
                verts[ib].normal[0] += nx * angleB;
                verts[ib].normal[1] += ny * angleB;
                verts[ib].normal[2] += nz * angleB;
                verts[ic].normal[0] += nx * angleC;
                verts[ic].normal[1] += ny * angleC;
                verts[ic].normal[2] += nz * angleC;
            }

            for (auto& v : verts)
            {
                const float len2 = v.normal[0]*v.normal[0] + v.normal[1]*v.normal[1] + v.normal[2]*v.normal[2];
                if (len2 > 1e-16f)
                {
                    const float inv = 1.f / std::sqrt(len2);
                    v.normal[0] *= inv; v.normal[1] *= inv; v.normal[2] *= inv;
                }
                else
                {
                    v.normal[0] = 0.f; v.normal[1] = 1.f; v.normal[2] = 0.f;
                }
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

        void Mat4MulVec3(const float m[16], const float in[3], float out[3])
        {
            out[0] = m[0]*in[0] + m[4]*in[1] + m[8]*in[2] + m[12];
            out[1] = m[1]*in[0] + m[5]*in[1] + m[9]*in[2] + m[13];
            out[2] = m[2]*in[0] + m[6]*in[1] + m[10]*in[2] + m[14];
        }

        void Mat3InverseTransposeMulVec3(const float m[16], const float in[3], float out[3])
        {
            const float a = m[0], b = m[4], c = m[8];
            const float d = m[1], e = m[5], f = m[9];
            const float g = m[2], h = m[6], i = m[10];
            const float A = e*i - f*h;
            const float B = f*g - d*i;
            const float C = d*h - e*g;
            const float D = c*h - b*i;
            const float E = a*i - c*g;
            const float F = b*g - a*h;
            const float G = b*f - c*e;
            const float H = c*d - a*f;
            const float I = a*e - b*d;
            const float det = a*A + b*B + c*C;
            const float invDet = 1.f / det;
            out[0] = (A*in[0] + D*in[1] + G*in[2]) * invDet;
            out[1] = (B*in[0] + E*in[1] + H*in[2]) * invDet;
            out[2] = (C*in[0] + F*in[1] + I*in[2]) * invDet;
        }

        // -------------------------------------------------------------------------
        // Bounding volume computation
        // -------------------------------------------------------------------------

        struct Bounds
        {
            float aabbMin[3];
            float aabbMax[3];
            float sphereCenter[3];
            float sphereRadius;
        };

        Bounds ComputeBounds(const std::vector<TempVertex>& verts)
        {
            Bounds bounds;
            if (verts.empty()) return bounds;

            bounds.aabbMin[0] = bounds.aabbMax[0] = verts[0].position[0];
            bounds.aabbMin[1] = bounds.aabbMax[1] = verts[0].position[1];
            bounds.aabbMin[2] = bounds.aabbMax[2] = verts[0].position[2];

            for (const auto& v : verts)
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

            for (const auto& v : verts)
            {
                const float dx = v.position[0] - bounds.sphereCenter[0];
                const float dy = v.position[1] - bounds.sphereCenter[1];
                const float dz = v.position[2] - bounds.sphereCenter[2];
                const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
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

            for (const auto& bone : sortedBones)
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
                    if (nodeIdx < 0 || seen[static_cast<std::size_t>(nodeIdx)]) continue;
                    seen[static_cast<std::size_t>(nodeIdx)] = true;

                    BoneInfo info;
                    info.name = SafeStr(data.nodes[static_cast<std::size_t>(nodeIdx)].name);
                    info.originalIndex = nodeIdx;
                    info.parentIndex = (data.nodes[static_cast<std::size_t>(nodeIdx)].parent)
                        ? ToIndex(data.nodes[static_cast<std::size_t>(nodeIdx)].parent, data) : -1;

                    // Read inverse bind matrix
                    if (skin.inverse_bind_matrices && ji < skin.inverse_bind_matrices->count)
                    {
                        cgltf_accessor_read_float(skin.inverse_bind_matrices, ji, info.ibm.data(), 16);
                    }
                    else
                    {
                        info.ibm = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
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
                return "default";
            const std::string name = SafeStr(data.materials[static_cast<std::size_t>(materialIndex)].name);
            return name.empty() ? "material_" + std::to_string(materialIndex) : name;
        }

    } // namespace

    // -------------------------------------------------------------------------

    ProcessedResult Process(
        const std::vector<std::byte>& gltfData,
        const std::filesystem::path&  sourcePath,
        const std::string&            virtualPath,
        const std::filesystem::path&  sourceDir)
    {
        cgltf_options options{};
        cgltf_data*   data = nullptr;

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

        // ── Collect and sort bones ──────────────────────────────────────────────
        std::vector<BoneInfo> bones = CollectBones(*data);

        // Build remap table: remapTable[originalIndex] = sortedIndex
        std::vector<uint32_t> remapTable(data->nodes_count, static_cast<uint32_t>(-1));
        uint64_t skelHash = 0;

        if (!bones.empty())
        {
            // Sort bones by name for deterministic hash
            std::stable_sort(bones.begin(), bones.end(),
                [](const BoneInfo& a, const BoneInfo& b) { return a.name < b.name; });

            // Build remap table
            for (uint32_t i = 0; i < static_cast<uint32_t>(bones.size()); ++i)
            {
                remapTable[static_cast<std::size_t>(bones[i].originalIndex)] = i;
            }

            // Compute skeleton hash
            skelHash = ComputeSkeletonHash(bones, remapTable);
            result.skeletonHash = std::to_string(skelHash);

            // ── Write .skel file ────────────────────────────────────────────────
            {
                const std::string skelName = Stem(sourcePath);
                SkelHeaderDisk hdr;
                hdr.boneCount = static_cast<uint32_t>(bones.size());
                hdr.nameLen = static_cast<uint16_t>(skelName.size());
                hdr.skeletonHash = skelHash;

                Append(result.skelData, hdr);
                AppendStringData(result.skelData, skelName);

                for (const auto& bone : bones)
                {
                    BoneEntryHeaderDisk boneHdr;
                    boneHdr.nameLen = static_cast<uint16_t>(bone.name.size());
                    Append(result.skelData, boneHdr);
                    AppendStringData(result.skelData, bone.name);

                    const int32_t remappedParent = (bone.parentIndex >= 0)
                        ? static_cast<int32_t>(remapTable[static_cast<std::size_t>(bone.parentIndex)]) : -1;
                    Append(result.skelData, remappedParent);
                    AppendBytes(result.skelData, bone.ibm.data(), sizeof(float) * 16);
                }
            }
        }

        // ── Eight-influences warning helper ─────────────────────────────────────
        auto WarnEightInfluences = [&](const cgltf_primitive& prim, const std::string& primDesc)
        {
            if (FindAttr(prim, cgltf_attribute_type_joints, 1))
            {
                std::cerr << "  MeshProcessor: WARNING - " << primDesc
                          << " has JOINTS_1/WEIGHTS_1 (8+ influences). "
                          << "Only the first 4 influences are stored.\n";
            }
        };

        // ── Count total primitives ──────────────────────────────────────────────
        uint32_t totalPrims = 0;
            for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
            {
                const cgltf_node& node = data->nodes[ni];
                if (!node.mesh) continue;

                float worldMat[16];
                cgltf_node_transform_world(&node, worldMat);

                for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
            {
                if (node.mesh->primitives[pi].type == cgltf_primitive_type_triangles &&
                    FindAttr(node.mesh->primitives[pi], cgltf_attribute_type_position))
                {
                    ++totalPrims;
                }
            }
        }

        // ── Collect unique material names for path refs ─────────────────────────
        // Only emit material paths where a corresponding .material directory exists.
        // If no .material file exists, the engine will use a default material.
        std::vector<std::string> materialPaths;
        {
            std::vector<bool> seen(data->materials_count, false);
            for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
            {
                const cgltf_node& node = data->nodes[ni];
                if (!node.mesh) continue;
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
                            materialPaths.push_back("materials/" + matName + ".material");
                        }
                    }
                }
            }
        }

        // ── Write .mesh file ────────────────────────────────────────────────────
        {
            std::vector<DiskMeshVertex> combinedVerts;
            std::vector<uint32_t> combinedIndices;
            uint32_t vertexOffset = 0;

            for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
            {
                const cgltf_node& node = data->nodes[ni];
                if (!node.mesh) continue;

                float worldMat[16];
                cgltf_node_transform_world(&node, worldMat);

                for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
                {
                    const cgltf_primitive& prim = node.mesh->primitives[pi];
                    if (prim.type != cgltf_primitive_type_triangles) continue;
                    const cgltf_accessor* posAcc = FindAttr(prim, cgltf_attribute_type_position, 0);
                    if (!posAcc) continue;

                    const uint32_t vertCount = static_cast<uint32_t>(posAcc->count);
                    const cgltf_accessor* normAcc = FindAttr(prim, cgltf_attribute_type_normal, 0);
                    const cgltf_accessor* tanAcc = FindAttr(prim, cgltf_attribute_type_tangent, 0);
                    const cgltf_accessor* uvAcc = FindAttr(prim, cgltf_attribute_type_texcoord, 0);
                    const cgltf_accessor* uv2Acc = FindAttr(prim, cgltf_attribute_type_texcoord, 1);
                    const cgltf_accessor* colorAcc = FindAttr(prim, cgltf_attribute_type_color, 0);
                    const cgltf_accessor* jointsAcc = FindAttr(prim, cgltf_attribute_type_joints, 0);
                    const cgltf_accessor* weightsAcc = FindAttr(prim, cgltf_attribute_type_weights, 0);

                    const bool isSkinned = (jointsAcc != nullptr);

                    std::vector<TempVertex> verts(vertCount);
                    std::array<cgltf_uint, 4> jointIdx{};

                    for (uint32_t v = 0; v < vertCount; ++v)
                    {
                        TempVertex& dst = verts[v];

                        if (isSkinned)
                        {
                            cgltf_accessor_read_float(posAcc, v, dst.position, 3);
                        }
                        else
                        {
                            float worldPos[3];
                            cgltf_accessor_read_float(posAcc, v, worldPos, 3);
                            Mat4MulVec3(worldMat, worldPos, dst.position);
                        }

                        if (normAcc)
                        {
                            cgltf_accessor_read_float(normAcc, v, dst.normal, 3);
                            if (!isSkinned)
                                Mat3InverseTransposeMulVec3(worldMat, dst.normal, dst.normal);
                        }

                        if (tanAcc)
                        {
                            cgltf_accessor_read_float(tanAcc, v, dst.tangent, 4);
                        }
                        else
                        {
                            dst.tangent[0] = 1.f; dst.tangent[1] = 0.f; dst.tangent[2] = 0.f; dst.tangent[3] = 1.f;
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
                            dst.color = 0xFFFFFFFF; // white
                        }

                        // Initialize joint indices to sentinel
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
                            for (int j = 0; j < 4; ++j)
                            {
                                if (jointIdx[j] < remapTable.size())
                                    dst.jointIndices[j] = remapTable[jointIdx[j]];
                                // else keep sentinel
                            }
                        }

                        if (weightsAcc)
                        {
                            cgltf_accessor_read_float(weightsAcc, v, dst.jointWeights, 4);

                            float wsum = dst.jointWeights[0] + dst.jointWeights[1]
                                       + dst.jointWeights[2] + dst.jointWeights[3];
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
                            indices[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k)) + vertexOffset;
                    }
                    else
                    {
                        indices.resize(vertCount);
                        for (uint32_t k = 0; k < vertCount; ++k) indices[k] = k + vertexOffset;
                    }

                    // Generate normals if missing
                    if (!normAcc) GenerateNormals(verts, indices);

                    // Convert to DiskMeshVertex
                    const std::size_t baseIdx = combinedVerts.size();
                    combinedVerts.resize(baseIdx + vertCount);
                    for (uint32_t v = 0; v < vertCount; ++v)
                    {
                        const TempVertex& src = verts[v];
                        DiskMeshVertex& dst = combinedVerts[baseIdx + v];
                        std::memcpy(dst.position, src.position, sizeof(src.position));
                        std::memcpy(dst.normal, src.normal, sizeof(src.normal));
                        std::memcpy(dst.tangent, src.tangent, sizeof(src.tangent));
                        std::memcpy(dst.uv, src.uv, sizeof(src.uv));
                        dst.color = src.color;
                        std::memcpy(dst.uv2, src.uv2, sizeof(src.uv2));
                        std::memset(dst._pad, 0, sizeof(dst._pad));
                        std::memcpy(dst.jointIndices, src.jointIndices, sizeof(src.jointIndices));
                        std::memcpy(dst.jointWeights, src.jointWeights, sizeof(src.jointWeights));
                    }

                    combinedIndices.insert(combinedIndices.end(), indices.begin(), indices.end());
                    vertexOffset += vertCount;
                }
            }

            // Compute bounds from populated vertex data
            Bounds bounds;
            if (!combinedVerts.empty())
            {
                bounds.aabbMin[0] = bounds.aabbMax[0] = combinedVerts[0].position[0];
                bounds.aabbMin[1] = bounds.aabbMax[1] = combinedVerts[0].position[1];
                bounds.aabbMin[2] = bounds.aabbMax[2] = combinedVerts[0].position[2];

                for (const auto& v : combinedVerts)
                {
                    bounds.aabbMin[0] = std::min(bounds.aabbMin[0], v.position[0]);
                    bounds.aabbMin[1] = std::min(bounds.aabbMin[1], v.position[1]);
                    bounds.aabbMin[2] = std::min(bounds.aabbMin[2], v.position[2]);
                    bounds.aabbMax[0] = std::max(bounds.aabbMax[0], v.position[0]);
                    bounds.aabbMax[1] = std::max(bounds.aabbMax[1], v.position[1]);
                    bounds.aabbMax[2] = std::max(bounds.aabbMax[2], v.position[2]);
                }

                // Ritter's bounding sphere (robust, near-optimal)
                {
                    const auto& verts = combinedVerts;
                    const std::size_t n = verts.size();
                    if (n > 0)
                    {
                        const float* P = verts[0].position;
                        std::size_t Q = 0;
                        float maxDistSq = 0.f;
                        for (std::size_t r = 1; r < n; ++r)
                        {
                            const float dx = verts[r].position[0] - P[0];
                            const float dy = verts[r].position[1] - P[1];
                            const float dz = verts[r].position[2] - P[2];
                            const float d = dx*dx + dy*dy + dz*dz;
                            if (d > maxDistSq) { maxDistSq = d; Q = r; }
                        }
                        const float* Qp = verts[Q].position;
                        std::size_t R = 0;
                        maxDistSq = 0.f;
                        for (std::size_t r = 0; r < n; ++r)
                        {
                            const float dx = verts[r].position[0] - Qp[0];
                            const float dy = verts[r].position[1] - Qp[1];
                            const float dz = verts[r].position[2] - Qp[2];
                            const float d = dx*dx + dy*dy + dz*dz;
                            if (d > maxDistSq) { maxDistSq = d; R = r; }
                        }
                        bounds.sphereCenter[0] = (Qp[0] + verts[R].position[0]) * 0.5f;
                        bounds.sphereCenter[1] = (Qp[1] + verts[R].position[1]) * 0.5f;
                        bounds.sphereCenter[2] = (Qp[2] + verts[R].position[2]) * 0.5f;
                        const float dx = verts[R].position[0] - bounds.sphereCenter[0];
                        const float dy = verts[R].position[1] - bounds.sphereCenter[1];
                        const float dz = verts[R].position[2] - bounds.sphereCenter[2];
                        bounds.sphereRadius = std::sqrt(dx*dx + dy*dy + dz*dz);
                        for (std::size_t r = 0; r < n; ++r)
                        {
                            const float vx = verts[r].position[0] - bounds.sphereCenter[0];
                            const float vy = verts[r].position[1] - bounds.sphereCenter[1];
                            const float vz = verts[r].position[2] - bounds.sphereCenter[2];
                            const float d = std::sqrt(vx*vx + vy*vy + vz*vz);
                            if (d > bounds.sphereRadius)
                            {
                                const float half = (d - bounds.sphereRadius) * 0.5f;
                                bounds.sphereRadius += half;
                                bounds.sphereCenter[0] += half * vx / d;
                                bounds.sphereCenter[1] += half * vy / d;
                                bounds.sphereCenter[2] += half * vz / d;
                            }
                        }
                    }
                }
            }

            // Skin ref path - same directory as source mesh in virtual path
            const std::string skinRefDir = std::filesystem::path(virtualPath).parent_path().generic_string();
            const std::string skinRefPath = bones.empty() ? "" : (skinRefDir.empty() ? (Stem(sourcePath) + ".skel") : (skinRefDir + "/" + Stem(sourcePath) + ".skel"));

            uint32_t maxIdx = 0;
            for (const auto& idx : combinedIndices)
                if (idx > maxIdx) maxIdx = idx;

            MeshHeaderDisk hdr;
            hdr.vertexCount = static_cast<uint32_t>(combinedVerts.size());
            hdr.indexCount = static_cast<uint32_t>(combinedIndices.size());
            hdr.skinRefPathLen = static_cast<uint32_t>(skinRefPath.size());
            hdr.materialCount = static_cast<uint32_t>(materialPaths.size());
            hdr.indexType = (maxIdx > 0xFFFF) ? 1 : 0;
            std::memcpy(hdr.aabbMin, bounds.aabbMin, sizeof(bounds.aabbMin));
            std::memcpy(hdr.aabbMax, bounds.aabbMax, sizeof(bounds.aabbMax));
            std::memcpy(hdr.sphereCenter, bounds.sphereCenter, sizeof(bounds.sphereCenter));
            hdr.sphereRadius = bounds.sphereRadius;

            Append(result.meshData, hdr);
            AppendBytes(result.meshData, combinedVerts.data(), combinedVerts.size() * sizeof(DiskMeshVertex));

            if (maxIdx > 0xFFFF)
            {
                AppendBytes(result.meshData, combinedIndices.data(), combinedIndices.size() * sizeof(uint32_t));
            }
            else
            {
                std::vector<uint16_t> idx16(combinedIndices.size());
                for (std::size_t i = 0; i < combinedIndices.size(); ++i)
                    idx16[i] = static_cast<uint16_t>(combinedIndices[i]);
                AppendBytes(result.meshData, idx16.data(), idx16.size() * sizeof(uint16_t));
            }
            AppendStringData(result.meshData, skinRefPath);
            for (const auto& matPath : materialPaths)
                AppendStr(result.meshData, matPath);
        }

        // ── Write .anim files and .animset ──────────────────────────────────────
        if (data->animations_count > 0 && !bones.empty())
        {
            std::vector<std::string> animPaths;

            for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
            {
                const cgltf_animation& anim = data->animations[ai];
                const std::string animName = SafeStr(anim.name);
                const std::string fileName = Stem(sourcePath) + "_" + (animName.empty() ? std::to_string(ai) : animName) + ".anim";

                std::vector<std::byte> animData;

                // Count valid channels
                uint32_t validChannels = 0;
                for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
                {
                    const auto& ch = anim.channels[ci];
                    if (!ch.sampler || !ch.target_node || !ch.sampler->input || !ch.sampler->output)
                        continue;
                    const int32_t targetNode = ToIndex(ch.target_node, *data);
                    if (targetNode < 0 || static_cast<std::size_t>(targetNode) >= remapTable.size())
                        continue;
                    if (remapTable[static_cast<std::size_t>(targetNode)] == static_cast<uint32_t>(-1))
                        continue;
                    ++validChannels;
                }

                AnimHeaderDisk animHdr;
                animHdr.channelCount = validChannels;
                animHdr.nameLen = static_cast<uint16_t>(animName.size());
                Append(animData, animHdr);
                AppendStringData(animData, animName);

                for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
                {
                    const cgltf_animation_channel& ch = anim.channels[ci];
                    if (!ch.sampler || !ch.target_node || !ch.sampler->input || !ch.sampler->output)
                        continue;

                    const int32_t targetNode = ToIndex(ch.target_node, *data);
                    if (targetNode < 0 || static_cast<std::size_t>(targetNode) >= remapTable.size())
                        continue;

                    const uint32_t remappedNode = remapTable[static_cast<std::size_t>(targetNode)];
                    if (remappedNode == static_cast<uint32_t>(-1))
                        continue;

                    AnimPathDisk path;
                    switch (ch.target_path)
                    {
                        case cgltf_animation_path_type_rotation:    path = AnimPathDisk::Rotation;    break;
                        case cgltf_animation_path_type_scale:       path = AnimPathDisk::Scale;       break;
                        case cgltf_animation_path_type_weights:     path = AnimPathDisk::Weights;     break;
                        default:                                     path = AnimPathDisk::Translation; break;
                    }

                    AnimInterpDisk interp;
                    switch (ch.sampler->interpolation)
                    {
                        case cgltf_interpolation_type_step:         interp = AnimInterpDisk::Step;        break;
                        case cgltf_interpolation_type_cubic_spline: interp = AnimInterpDisk::CubicSpline; break;
                        default:                                     interp = AnimInterpDisk::Linear;      break;
                    }

                    const cgltf_accessor* inputAcc  = ch.sampler->input;
                    const cgltf_accessor* outputAcc = ch.sampler->output;
                    const uint32_t keyCount = static_cast<uint32_t>(std::min(inputAcc->count, outputAcc->count));

                    ChannelHeaderDisk chHdr;
                    chHdr.nodeIndex = remappedNode;
                    chHdr.path = static_cast<uint8_t>(path);
                    chHdr.interp = static_cast<uint8_t>(interp);
                    chHdr.keyCount = keyCount;
                    Append(animData, chHdr);

                    // Times
                    std::array<float, 4> val{};
                    for (uint32_t k = 0; k < keyCount; ++k)
                    {
                        cgltf_accessor_read_float(inputAcc, k, val.data(), 1);
                        Append(animData, val[0]);
                    }

                    // Values - vec4 per key
                    const bool isRotation = (path == AnimPathDisk::Rotation);
                    const uint32_t compCount = isRotation ? 4 : 3;
                    for (uint32_t k = 0; k < keyCount; ++k)
                    {
                        val[3] = 0.f;
                        cgltf_accessor_read_float(outputAcc, k, val.data(), compCount);
                        AppendBytes(animData, val.data(), sizeof(float) * 4);
                    }
                }

                result.animFiles.emplace_back(fileName, std::move(animData));
                animPaths.push_back("animations/" + fileName);
            }

            // Write .animset
            AnimSetHeaderDisk setHdr;
            setHdr.animCount = static_cast<uint32_t>(animPaths.size());
            setHdr.skeletonHash = skelHash;

            Append(result.animsetData, setHdr);
            for (const auto& animPath : animPaths)
            {
                AppendStr(result.animsetData, animPath);
            }
        }

        cgltf_free(data);
        return result;
    }
} // namespace MeshProcessor
