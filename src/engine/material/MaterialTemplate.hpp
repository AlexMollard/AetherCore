#pragma once

#include <cstdint>
#include <string_view>

#include "gpu/GpuTypes.hpp"
#include "utils/Hash.hpp"

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
		utils::Fnv1aHasher hasher;
		hasher.Mix(t.shaderVfsPath);
		hasher.Mix(t.fragmentVfsPath);
		hasher.MixValue(static_cast<std::uint32_t>(t.cullMode));
		hasher.MixValue(t.blendEnable);
		hasher.MixValue(t.depthWriteEnable);
		return hasher.Value();
	}
} // namespace aether
