#pragma once

#include "scripting/interop/InteropCommon.hpp"

#if !defined(__EMSCRIPTEN__)
#	include "net/NetworkContext.hpp"
#	include "utils/ServiceContainer.hpp"
#endif

namespace aether::app::scripting::interop
{
	// Whether this process may write simulation state (velocity, force, character
	// input, motion type) on `id`. Single-player and offline sessions own everything.
	// The browser runtime has no networking at all, so every entity is locally owned
	// there - the same answer native gives with no NetworkContext registered.
	[[nodiscard]] inline bool CanControl(std::uint32_t id)
	{
#if defined(__EMSCRIPTEN__)
		(void) id;
		return true;
#else
		const auto& ctx = ActiveContext();
		if (ctx.services == nullptr)
		{
			return true;
		}
		const auto* network = ctx.services->TryGet<aether::net::NetworkContext>();
		return network == nullptr || network->HasAuthority(ActiveWorld(), aether::Entity{id});
#endif
	}
} // namespace aether::app::scripting::interop
