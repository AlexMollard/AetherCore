#include "MeshProcessor.hpp"

#include <AeBnFormat.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

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
            AppendBytes(buf, s.data(), s.size());
        }

        // -------------------------------------------------------------------------
        // Index helpers
        // -------------------------------------------------------------------------

        static int32_t ToIndex(const cgltf_node* value, const cgltf_data& data)
        {
            if (!value || !data.nodes) return -1;
            return static_cast<int32_t>(value - data.nodes);
        }
        static int32_t ToIndex(const cgltf_mesh* value, const cgltf_data& data)
        {
            if (!value || !data.meshes) return -1;
            return static_cast<int32_t>(value - data.meshes);
        }
        static int32_t ToIndex(const cgltf_skin* value, const cgltf_data& data)
        {
            if (!value || !data.skins) return -1;
            return static_cast<int32_t>(value - data.skins);
        }
        static int32_t ToIndex(const cgltf_material* value, const cgltf_data& data)
        {
            if (!value || !data.materials) return -1;
            return static_cast<int32_t>(value - data.materials);
        }
        static int32_t ToIndex(const cgltf_image* value, const cgltf_data& data)
        {
            if (!value || !data.images) return -1;
            return static_cast<int32_t>(value - data.images);
        }
        static int32_t ToIndex(const cgltf_texture* value, const cgltf_data& data)
        {
            if (!value || !data.textures) return -1;
            return static_cast<int32_t>(value - data.textures);
        }

        static std::string SafeStr(const char* s) { return s ? s : ""; }

        // -------------------------------------------------------------------------
        // Image URI: replace extension with ".texture"
        // -------------------------------------------------------------------------

        std::string TextureUri(const char* rawUri)
        {
            if (!rawUri) return {};
            const std::filesystem::path p(rawUri);
            return (p.parent_path() / p.stem()).generic_string() + ".texture";
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
        // Normal generation (used when the primitive has no NORMAL attribute)
        // -------------------------------------------------------------------------

        void GenerateNormals(std::vector<AeBnVertex>& verts, const std::vector<uint32_t>& idx)
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
                const float len2 = nx*nx + ny*ny + nz*nz;
                if (len2 < 1e-16f) continue;

                for (uint32_t j : {ia, ib, ic})
                {
                    verts[j].normal[0] += nx;
                    verts[j].normal[1] += ny;
                    verts[j].normal[2] += nz;
                }
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
    } // namespace

    // -------------------------------------------------------------------------

    std::vector<std::byte> ToBinary(
        const std::vector<std::byte>& gltfData,
        const std::filesystem::path&  sourcePath)
    {
        cgltf_options options{};
        cgltf_data*   data = nullptr;

        if (cgltf_parse(&options, gltfData.data(), gltfData.size(), &data) != cgltf_result_success)
        {
            std::cerr << "  MeshProcessor: cgltf_parse failed for " << sourcePath << "\n";
            return {};
        }

        const std::string srcPathStr = sourcePath.string();
        cgltf_load_buffers(&options, data, srcPathStr.c_str());

        if (cgltf_validate(data) != cgltf_result_success)
        {
            std::cerr << "  MeshProcessor: cgltf_validate failed for " << sourcePath << "\n";
            cgltf_free(data);
            return {};
        }

        // ── Count total primitives ──────────────────────────────────────────────
        uint32_t totalPrims = 0;
        for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
        {
            const cgltf_node& node = data->nodes[ni];
            if (!node.mesh) continue;
            for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
            {
                if (node.mesh->primitives[pi].type == cgltf_primitive_type_triangles &&
                    FindAttr(node.mesh->primitives[pi], cgltf_attribute_type_position))
                {
                    ++totalPrims;
                }
            }
        }

        // ── File header ─────────────────────────────────────────────────────────
        AeBnHeader fileHdr;
        fileHdr.imageCount     = static_cast<uint32_t>(data->images_count);
        fileHdr.textureCount   = static_cast<uint32_t>(data->textures_count);
        fileHdr.materialCount  = static_cast<uint32_t>(data->materials_count);
        fileHdr.nodeCount      = static_cast<uint32_t>(data->nodes_count);
        fileHdr.skinCount      = static_cast<uint32_t>(data->skins_count);
        fileHdr.primitiveCount = totalPrims;
        fileHdr.animCount      = static_cast<uint32_t>(data->animations_count);

        std::vector<std::byte> out;
        out.reserve(gltfData.size() * 2);
        Append(out, fileHdr);

        // ── Images ──────────────────────────────────────────────────────────────
        for (cgltf_size i = 0; i < data->images_count; ++i)
        {
            const cgltf_image& img = data->images[i];
            const std::string name = SafeStr(img.name);
            const std::string uri  = TextureUri(img.uri);

            AeBnImageHeader hdr;
            hdr.nameLen = static_cast<uint16_t>(name.size());
            hdr.uriLen  = static_cast<uint16_t>(uri.size());
            Append(out, hdr);
            AppendStr(out, name);
            AppendStr(out, uri);
        }

        // ── Textures ────────────────────────────────────────────────────────────
        for (cgltf_size i = 0; i < data->textures_count; ++i)
        {
            const cgltf_texture& tex = data->textures[i];
            const std::string name = SafeStr(tex.name);

            AeBnTextureHeader hdr;
            hdr.imageIndex = ToIndex(tex.image, *data);
            hdr.nameLen    = static_cast<uint16_t>(name.size());
            Append(out, hdr);
            AppendStr(out, name);
        }

        // ── Materials ───────────────────────────────────────────────────────────
        for (cgltf_size i = 0; i < data->materials_count; ++i)
        {
            const cgltf_material& mat = data->materials[i];
            const std::string name = SafeStr(mat.name);

            AeBnMaterialHeader hdr{};
            const auto& pbr = mat.pbr_metallic_roughness;
            for (int k = 0; k < 4; ++k) hdr.baseColorFactor[k]  = static_cast<float>(pbr.base_color_factor[k]);
            hdr.metallicFactor  = static_cast<float>(pbr.metallic_factor);
            hdr.roughnessFactor = static_cast<float>(pbr.roughness_factor);
            for (int k = 0; k < 3; ++k) hdr.emissiveFactor[k] = static_cast<float>(mat.emissive_factor[k]);
            hdr.alphaCutoff              = static_cast<float>(mat.alpha_cutoff);
            hdr.baseColorTexture         = ToIndex(pbr.base_color_texture.texture, *data);
            hdr.metallicRoughnessTexture = ToIndex(pbr.metallic_roughness_texture.texture, *data);
            hdr.normalTexture            = ToIndex(mat.normal_texture.texture, *data);
            hdr.occlusionTexture         = ToIndex(mat.occlusion_texture.texture, *data);
            hdr.emissiveTexture          = ToIndex(mat.emissive_texture.texture, *data);
            hdr.doubleSided = mat.double_sided ? 1 : 0;
            hdr.alphaBlend  = (mat.alpha_mode == cgltf_alpha_mode_blend) ? 1 : 0;
            hdr.alphaMask   = (mat.alpha_mode == cgltf_alpha_mode_mask)  ? 1 : 0;
            hdr.nameLen     = static_cast<uint16_t>(name.size());
            Append(out, hdr);
            AppendStr(out, name);
        }

        // ── Nodes ───────────────────────────────────────────────────────────────
        for (cgltf_size i = 0; i < data->nodes_count; ++i)
        {
            const cgltf_node& node = data->nodes[i];
            const std::string name = SafeStr(node.name);

            AeBnNodeHeader hdr{};
            hdr.parentIndex = (node.parent) ? ToIndex(node.parent, *data) : -1;
            hdr.meshIndex   = ToIndex(node.mesh, *data);
            hdr.skinIndex   = ToIndex(node.skin, *data);

            if (node.has_translation)
            {
                for (int k = 0; k < 3; ++k) hdr.translation[k] = static_cast<float>(node.translation[k]);
            }
            // Default rotation: identity quaternion stored as xyzw = (0,0,0,1)
            hdr.rotation[0] = 0.f; hdr.rotation[1] = 0.f; hdr.rotation[2] = 0.f; hdr.rotation[3] = 1.f;
            if (node.has_rotation)
            {
                for (int k = 0; k < 4; ++k) hdr.rotation[k] = static_cast<float>(node.rotation[k]);
            }
            hdr.scale[0] = hdr.scale[1] = hdr.scale[2] = 1.f;
            if (node.has_scale)
            {
                for (int k = 0; k < 3; ++k) hdr.scale[k] = static_cast<float>(node.scale[k]);
            }
            if (node.has_matrix)
            {
                hdr.hasMatrix = 1;
                for (int k = 0; k < 16; ++k) hdr.matrix[k] = static_cast<float>(node.matrix[k]);
            }
            hdr.childCount = static_cast<uint32_t>(node.children_count);
            hdr.nameLen    = static_cast<uint16_t>(name.size());

            Append(out, hdr);
            for (cgltf_size c = 0; c < node.children_count; ++c)
            {
                const uint32_t childIdx = static_cast<uint32_t>(ToIndex(node.children[c], *data));
                Append(out, childIdx);
            }
            AppendStr(out, name);
        }

        // ── Skins ───────────────────────────────────────────────────────────────
        for (cgltf_size i = 0; i < data->skins_count; ++i)
        {
            const cgltf_skin& skin = data->skins[i];
            const std::string name = SafeStr(skin.name);

            AeBnSkinHeader hdr;
            hdr.skeletonRoot = ToIndex(skin.skeleton, *data);
            hdr.jointCount   = static_cast<uint32_t>(skin.joints_count);
            hdr.nameLen      = static_cast<uint16_t>(name.size());
            Append(out, hdr);
            AppendStr(out, name);

            for (cgltf_size j = 0; j < skin.joints_count; ++j)
            {
                const uint32_t jointIdx = static_cast<uint32_t>(ToIndex(skin.joints[j], *data));
                Append(out, jointIdx);
            }

            if (skin.inverse_bind_matrices)
            {
                std::array<float, 16> mtx{};
                for (cgltf_size m = 0; m < skin.inverse_bind_matrices->count; ++m)
                {
                    cgltf_accessor_read_float(skin.inverse_bind_matrices, m, mtx.data(), 16);
                    AppendBytes(out, mtx.data(), sizeof(mtx));
                }
            }
            else
            {
                // Write identity matrices for each joint
                const std::array<float, 16> identity = {
                    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1
                };
                for (uint32_t j = 0; j < skin.joints_count; ++j)
                    AppendBytes(out, identity.data(), sizeof(identity));
            }
        }

        // ── Primitives ──────────────────────────────────────────────────────────
        for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
        {
            const cgltf_node& node = data->nodes[ni];
            if (!node.mesh) continue;

            for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi)
            {
                const cgltf_primitive& prim = node.mesh->primitives[pi];
                if (prim.type != cgltf_primitive_type_triangles) continue;

                const cgltf_accessor* posAcc     = FindAttr(prim, cgltf_attribute_type_position, 0);
                if (!posAcc) continue;

                const cgltf_accessor* normAcc    = FindAttr(prim, cgltf_attribute_type_normal, 0);
                const cgltf_accessor* tanAcc     = FindAttr(prim, cgltf_attribute_type_tangent, 0);
                const cgltf_accessor* uvAcc      = FindAttr(prim, cgltf_attribute_type_texcoord, 0);
                if (!uvAcc) uvAcc = FindAttr(prim, cgltf_attribute_type_texcoord, 1);
                const cgltf_accessor* colorAcc   = FindAttr(prim, cgltf_attribute_type_color, 0);
                const cgltf_accessor* jointsAcc  = FindAttr(prim, cgltf_attribute_type_joints, 0);
                const cgltf_accessor* weightsAcc = FindAttr(prim, cgltf_attribute_type_weights, 0);

                const uint32_t vertCount = static_cast<uint32_t>(posAcc->count);

                std::vector<AeBnVertex> verts(vertCount);
                std::array<float, 4>       fv{};
                std::array<cgltf_uint, 4>  uv{};

                for (uint32_t v = 0; v < vertCount; ++v)
                {
                    AeBnVertex& dst = verts[v];

                    cgltf_accessor_read_float(posAcc, v, fv.data(), 3);
                    dst.position[0] = fv[0]; dst.position[1] = fv[1]; dst.position[2] = fv[2];

                    if (normAcc)
                    {
                        cgltf_accessor_read_float(normAcc, v, fv.data(), 3);
                        dst.normal[0] = fv[0]; dst.normal[1] = fv[1]; dst.normal[2] = fv[2];
                    }

                    if (tanAcc)
                    {
                        cgltf_accessor_read_float(tanAcc, v, fv.data(), 4);
                        dst.tangent[0] = fv[0]; dst.tangent[1] = fv[1]; dst.tangent[2] = fv[2]; dst.tangent[3] = fv[3];
                    }
                    else
                    {
                        dst.tangent[0] = 1.f; dst.tangent[1] = 0.f; dst.tangent[2] = 0.f; dst.tangent[3] = 1.f;
                    }

                    if (uvAcc)
                    {
                        cgltf_accessor_read_float(uvAcc, v, fv.data(), 2);
                        dst.uv[0] = fv[0]; dst.uv[1] = fv[1];
                    }

                    if (colorAcc)
                    {
                        cgltf_accessor_read_float(colorAcc, v, fv.data(), 4);
                        dst.color[0] = fv[0]; dst.color[1] = fv[1]; dst.color[2] = fv[2];
                    }
                    else
                    {
                        dst.color[0] = dst.color[1] = dst.color[2] = 1.f;
                    }

                    if (jointsAcc)
                    {
                        cgltf_accessor_read_uint(jointsAcc, v, uv.data(), 4);
                        dst.jointIndices[0] = uv[0]; dst.jointIndices[1] = uv[1];
                        dst.jointIndices[2] = uv[2]; dst.jointIndices[3] = uv[3];
                    }

                    if (weightsAcc)
                    {
                        cgltf_accessor_read_float(weightsAcc, v, fv.data(), 4);
                        dst.jointWeights[0] = fv[0]; dst.jointWeights[1] = fv[1];
                        dst.jointWeights[2] = fv[2]; dst.jointWeights[3] = fv[3];
                    }
                    else
                    {
                        dst.jointWeights[0] = 1.f;
                        dst.jointWeights[1] = dst.jointWeights[2] = dst.jointWeights[3] = 0.f;
                    }
                }

                // Build index buffer
                std::vector<uint32_t> indices;
                if (prim.indices)
                {
                    indices.resize(prim.indices->count);
                    for (cgltf_size k = 0; k < prim.indices->count; ++k)
                        indices[k] = static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, k));
                }
                else
                {
                    indices.resize(vertCount);
                    for (uint32_t k = 0; k < vertCount; ++k) indices[k] = k;
                }

                // Generate normals if missing (matches GltfAsset.cpp behaviour)
                if (!normAcc) GenerateNormals(verts, indices);

                // Fix near-black color streams (matches GltfAsset.cpp behaviour)
                if (colorAcc)
                {
                    float maxC = 0.f;
                    for (const auto& vtx : verts)
                        maxC = std::max(maxC, std::max(vtx.color[0], std::max(vtx.color[1], vtx.color[2])));
                    if (maxC < 0.01f)
                        for (auto& vtx : verts)
                            vtx.color[0] = vtx.color[1] = vtx.color[2] = 1.f;
                }

                AeBnPrimitiveHeader hdr;
                hdr.nodeIndex     = static_cast<uint32_t>(ni);
                hdr.materialIndex = ToIndex(prim.material, *data);
                hdr.skinIndex     = ToIndex(node.skin, *data);
                hdr.vertexCount   = vertCount;
                hdr.indexCount    = static_cast<uint32_t>(indices.size());

                Append(out, hdr);
                AppendBytes(out, verts.data(), verts.size() * sizeof(AeBnVertex));
                AppendBytes(out, indices.data(), indices.size() * sizeof(uint32_t));
            }
        }

        // ── Animations ──────────────────────────────────────────────────────────
        for (cgltf_size ai = 0; ai < data->animations_count; ++ai)
        {
            const cgltf_animation& anim = data->animations[ai];
            const std::string animName = SafeStr(anim.name);

            // Count valid channels first
            uint32_t validChannels = 0;
            for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
            {
                const auto& ch = anim.channels[ci];
                if (ch.sampler && ch.target_node && ch.sampler->input && ch.sampler->output)
                    ++validChannels;
            }

            AeBnAnimHeader animHdr;
            animHdr.channelCount = validChannels;
            animHdr.nameLen      = static_cast<uint16_t>(animName.size());
            Append(out, animHdr);
            AppendStr(out, animName);

            for (cgltf_size ci = 0; ci < anim.channels_count; ++ci)
            {
                const cgltf_animation_channel& ch = anim.channels[ci];
                if (!ch.sampler || !ch.target_node || !ch.sampler->input || !ch.sampler->output)
                    continue;

                const int32_t targetNode = ToIndex(ch.target_node, *data);
                if (targetNode < 0) continue;

                AeBnAnimPath path;
                switch (ch.target_path)
                {
                    case cgltf_animation_path_type_rotation:    path = AeBnAnimPath::Rotation;    break;
                    case cgltf_animation_path_type_scale:       path = AeBnAnimPath::Scale;       break;
                    case cgltf_animation_path_type_weights:     path = AeBnAnimPath::Weights;     break;
                    default:                                     path = AeBnAnimPath::Translation; break;
                }

                AeBnInterp interp;
                switch (ch.sampler->interpolation)
                {
                    case cgltf_interpolation_type_step:         interp = AeBnInterp::Step;        break;
                    case cgltf_interpolation_type_cubic_spline: interp = AeBnInterp::CubicSpline; break;
                    default:                                     interp = AeBnInterp::Linear;      break;
                }

                const cgltf_accessor* inputAcc  = ch.sampler->input;
                const cgltf_accessor* outputAcc = ch.sampler->output;
                const uint32_t keyCount = static_cast<uint32_t>(std::min(inputAcc->count, outputAcc->count));

                AeBnChannelHeader chHdr;
                chHdr.nodeIndex = static_cast<uint32_t>(targetNode);
                chHdr.path      = static_cast<uint8_t>(path);
                chHdr.interp    = static_cast<uint8_t>(interp);
                chHdr._pad[0]   = chHdr._pad[1] = 0;
                chHdr.keyCount  = keyCount;
                Append(out, chHdr);

                // Times
                std::array<float, 4> val{};
                for (uint32_t k = 0; k < keyCount; ++k)
                {
                    cgltf_accessor_read_float(inputAcc, k, val.data(), 1);
                    Append(out, val[0]);
                }

                // Values - vec4 per key (Translation/Scale: w=0, Rotation: xyzw)
                const bool isRotation = (path == AeBnAnimPath::Rotation);
                const uint32_t compCount = isRotation ? 4 : 3;
                for (uint32_t k = 0; k < keyCount; ++k)
                {
                    val[3] = 0.f;
                    cgltf_accessor_read_float(outputAcc, k, val.data(), compCount);
                    AppendBytes(out, val.data(), sizeof(float) * 4);
                }
            }
        }

        cgltf_free(data);
        return out;
    }
} // namespace MeshProcessor
