#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include <glm/glm.hpp>

#include "rendering/CustomPassRegistry.hpp"
#include "scripting/GameplayServices.h"

namespace aether::scripting
{
	// Shared execution of the custom-pass gameplay service operations (14 register, 15 unregister,
	// 16 submit) against the game-thread CustomPassRegistry. The native Rust services and the browser
	// host both route here so a pass behaves identically on every backend. Game thread only.
	[[nodiscard]] inline bool IsCustomPassOperation(std::uint32_t operation) noexcept
	{
		return operation >= AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS && operation <= AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS;
	}

	[[nodiscard]] inline AetherGameplayStatus ExecuteCustomPassOperation(CustomPassRegistry& registry, const AetherGameplayServiceRequest& request)
	{
		constexpr std::uint32_t kMaxNameBytes = 256;
		if (request.text.length == 0 || request.text.length > kMaxNameBytes || request.text.data == nullptr)
		{
			return AE_GAMEPLAY_INVALID_ARGUMENT;
		}
		const std::string name(request.text.data, request.text.length);
		switch (request.operation)
		{
			case AE_GAMEPLAY_SERVICE_REGISTER_CUSTOM_PASS:
				if (request.text2.length == 0 || request.text2.length > kMaxNameBytes || request.text2.data == nullptr || request.integer < 0 || request.integer > static_cast<std::int64_t>(CustomPassStage::EmissiveOverLight2D))
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				registry.Register(name, std::string(request.text2.data, request.text2.length), static_cast<CustomPassStage>(request.integer));
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_UNREGISTER_CUSTOM_PASS:
				registry.Unregister(name);
				return AE_GAMEPLAY_OK;
			case AE_GAMEPLAY_SERVICE_SUBMIT_CUSTOM_PASS:
			{
				constexpr std::size_t kVec4Bytes = sizeof(float) * 4;
				constexpr std::size_t kHeader = AE_GAMEPLAY_CUSTOM_PASS_HEADER_VEC4;
				const std::size_t bytes = request.text2.length;
				if (request.text2.data == nullptr || bytes % kVec4Bytes != 0 || bytes / kVec4Bytes < kHeader)
				{
					return AE_GAMEPLAY_INVALID_ARGUMENT;
				}
				const auto* payload = request.text2.data;
				const auto read = [payload](std::size_t index)
				{
					glm::vec4 value;
					std::memcpy(&value, payload + index * kVec4Bytes, kVec4Bytes);
					return value;
				};
				const std::size_t count = std::min<std::size_t>(bytes / kVec4Bytes - kHeader, AE_GAMEPLAY_CUSTOM_PASS_MAX_VEC4);
				registry.SubmitIndexed(name, count, [&read](std::size_t index) { return read(kHeader + index); }, read(0), read(1), read(2));
				return AE_GAMEPLAY_OK;
			}
			default:
				return AE_GAMEPLAY_UNSUPPORTED;
		}
	}
} // namespace aether::scripting
