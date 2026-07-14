#pragma once

#include <cstdint>
#include <string_view>

#include "gpu/GpuTypes.hpp"

namespace aether
{
	// shaderVfsPath / fragmentVfsPath must reference storage that outlives the cache
	struct MaterialTemplate
	{
		std::string_view shaderVfsPath;
		std::string_view fragmentVfsPath;
		gpu::CullMode cullMode = gpu::CullMode::None;
		bool blendEnable = false;
		bool depthWriteEnable = true;

		friend bool operator==(const MaterialTemplate& a, const MaterialTemplate& b)
		{
			return a.shaderVfsPath == b.shaderVfsPath && a.fragmentVfsPath == b.fragmentVfsPath && a.cullMode == b.cullMode && a.blendEnable == b.blendEnable && a.depthWriteEnable == b.depthWriteEnable;
		}
	};

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
		const auto cull = static_cast<std::uint32_t>(t.cullMode);
		h = mix(h, &cull, sizeof(cull));
		h = mix(h, &t.blendEnable, sizeof(t.blendEnable));
		h = mix(h, &t.depthWriteEnable, sizeof(t.depthWriteEnable));
		return h;
	}
} // namespace aether
