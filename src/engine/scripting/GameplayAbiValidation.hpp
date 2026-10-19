#pragma once

#include "scripting/GameplayAbi.h"

namespace aether::scripting
{
	[[nodiscard]] AetherGameplayStatus ValidateGameplayModule(const AetherGameplayModuleApi& api) noexcept;
	[[nodiscard]] AetherGameplayStatus ValidateGameplayEngine(const AetherGameplayEngineApi& api) noexcept;
	// Both helpers clear only min(outSize, sizeof(table)) bytes, including every failure path.
	[[nodiscard]] AetherGameplayStatus QueryGameplayModule(AetherGameplayQuery query, uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi) noexcept;
	[[nodiscard]] AetherGameplayStatus PublishGameplayModule(const AetherGameplayModuleApi& source, uint32_t requestedMajor, uint32_t outSize, AetherGameplayModuleApi* outApi) noexcept;
	[[nodiscard]] bool IsGameplayUtf8(AetherGameplayUtf8 value) noexcept;
	[[nodiscard]] AetherGameplayStatus CopyGameplayUtf8(AetherGameplayUtf8 source, AetherGameplayUtf8Buffer destination) noexcept;
	[[nodiscard]] AetherGameplayStatus DispatchGameplayEvent(const AetherGameplayModuleApi& api, AetherGameplayInstance instance, const AetherGameplayEvent& event) noexcept;
} // namespace aether::scripting
