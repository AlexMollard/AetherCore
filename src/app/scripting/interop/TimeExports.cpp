#include "scripting/interop/InteropCommon.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

AE_SCRIPT_API float aether_time_total()
{
	return ActiveContext().elapsedTime;
}

AE_SCRIPT_API float aether_time_delta()
{
	return ActiveContext().deltaTime;
}

AE_SCRIPT_API std::int64_t aether_time_frame_count()
{
	return static_cast<std::int64_t>(ActiveContext().frameCount);
}
