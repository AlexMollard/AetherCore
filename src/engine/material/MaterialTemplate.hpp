#pragma once

#include <cstdint>
#include <string_view>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	// The axes that force a distinct GraphicsPipeline object (spec §3.1): shader
	// program + raster/blend/depth-write state. A subset of GraphicsPipeline::Desc;
	// the format/heap-mapping axes are frame-graph-constant and live in
	// PipelineCache::Context, not here. Content-addressed and deduped by
	// PipelineCache -- the render-state analogue of the 80-byte GpuMaterial record.
	//
	// shaderVfsPath / fragmentVfsPath must reference storage that outlives the cache
	// (today: static VFS path literals like "shaders://gltf_mesh.spv").
	struct MaterialTemplate
	{
		std::string_view shaderVfsPath;               // e.g. "shaders://gltf_mesh.spv"
		std::string_view fragmentVfsPath;             // usually empty (shared vertex+fragment module)
		gpu::CullMode cullMode = gpu::CullMode::None; // authored doubleSided maps here
		bool blendEnable = false;                     // authored alphaBlend maps here
		bool depthWriteEnable = true;

		friend bool operator==(const MaterialTemplate& a, const MaterialTemplate& b)
		{
			return a.shaderVfsPath == b.shaderVfsPath && a.fragmentVfsPath == b.fragmentVfsPath && a.cullMode == b.cullMode && a.blendEnable == b.blendEnable && a.depthWriteEnable == b.depthWriteEnable;
		}
	};

	// FNV-1a over the template's semantic fields, mirroring MaterialRegistry's
	// hashing style. Collisions are resolved by operator== on a hit, so hash
	// quality affects only dedup speed, not correctness.
	[[nodiscard]] inline std::uint64_t HashMaterialTemplate(const MaterialTemplate& t)
	{
		auto mix = [](std::uint64_t h, const void* data, std::size_t n)
		{
			const auto* p = static_cast<const unsigned char*>(data);
			for (std::size_t i = 0; i < n; ++i)
			{
				h ^= p[i];
				h *= 1099511628211ull;
			}
			return h;
		};
		std::uint64_t h = 1469598103934665603ull;
		h = mix(h, t.shaderVfsPath.data(), t.shaderVfsPath.size());
		h = mix(h, t.fragmentVfsPath.data(), t.fragmentVfsPath.size());
		const std::uint32_t cull = static_cast<std::uint32_t>(t.cullMode);
		h = mix(h, &cull, sizeof(cull));
		h = mix(h, &t.blendEnable, sizeof(t.blendEnable));
		h = mix(h, &t.depthWriteEnable, sizeof(t.depthWriteEnable));
		return h;
	}
} // namespace aether
