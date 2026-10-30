#pragma once

#include "scripting/interop/InteropCommon.hpp"

#include "net/NetworkContext.hpp"
#include "utils/ServiceContainer.hpp"

namespace aether::app::scripting::interop
{
	// Whether this process may write simulation state (velocity, force, character
	// input, motion type) on `id`. Single-player and offline sessions own everything.
	// A missing network context is the single-player/offline case on every platform.
	[[nodiscard]] inline bool CanControl(std::uint32_t id)
	{
		const auto& ctx = ActiveContext();
		if (ctx.services == nullptr)
		{
			return true;
		}
		const auto* network = ctx.services->TryGet<aether::net::NetworkContext>();
		return network == nullptr || network->HasAuthority(ActiveWorld(), aether::Entity{id});
	}
} // namespace aether::app::scripting::interop
