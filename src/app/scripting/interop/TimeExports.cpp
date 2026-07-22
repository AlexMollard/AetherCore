#include "scripting/interop/InteropCommon.hpp"

#include "IEngineRuntime.hpp"
#include "PlayState.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API float aether_time_total()
{
	return ActiveContext().elapsedTime;
}

// Real (wall-clock) seconds since the frame loop started, ignoring the play-speed / pause time
// scale - so UI can keep animating while the game is frozen (e.g. the pause menu). Falls back to
// the scaled clock if the engine runtime is unavailable (e.g. outside a play session).
AE_SCRIPT_API float aether_time_unscaled()
{
	const auto& ctx = ActiveContext();
	return ctx.engineRuntime != nullptr ? static_cast<float>(ctx.engineRuntime->RealElapsedSeconds()) : ctx.elapsedTime;
}

AE_SCRIPT_API float aether_time_delta()
{
	return ActiveContext().deltaTime;
}

AE_SCRIPT_API std::int64_t aether_time_frame_count()
{
	return static_cast<std::int64_t>(ActiveContext().frameCount);
}

// Global play-speed / pause. Setting 0 freezes the simulation (physics, particles,
// animation all advance by a zero delta) while the script system keeps ticking -
// so an in-game pause menu can still read input to resume. Takes effect next frame
// (the current frame's gameDt is already computed).
AE_SCRIPT_API void aether_time_set_scale(float scale)
{
	if (auto* services = ActiveContext().services)
	{
		if (auto* play = services->TryGet<aether::app::PlayState>())
		{
			play->SetTimeScale(scale);
		}
	}
}

AE_SCRIPT_API float aether_time_get_scale()
{
	if (auto* services = ActiveContext().services)
	{
		if (auto* play = services->TryGet<aether::app::PlayState>())
		{
			return play->TimeScale();
		}
	}
	return 1.0f;
}
