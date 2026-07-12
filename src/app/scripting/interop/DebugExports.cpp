#include "scripting/interop/InteropCommon.hpp"

#include "physics/PhysicsDebugRenderer.hpp"

// Debug draw exported to C#. Appends immediate-mode world-space primitives to the
// engine's per-frame debug-vertex list (extracted into the RenderFramePacket and
// drawn by the $Debug pass when debug rendering is enabled - like Unity's
// Debug.DrawLine showing with gizmos).

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// Master switch for immediate debug drawing (the $Debug pass). Off by default,
// like Unity's gizmos - a script turns it on to see its Debug.Draw* output.
AE_SCRIPT_API void aether_debug_set_enabled(std::int32_t enabled)
{
	aether::SetDebugRenderingEnabled(enabled != 0);
}

AE_SCRIPT_API std::int32_t aether_debug_is_enabled()
{
	return aether::IsDebugRenderingEnabled() ? 1 : 0;
}

AE_SCRIPT_API void aether_debug_draw_line(Vec3 a, Vec3 b, Vec4 color)
{
	if (auto* v = ActiveContext().debugVertices)
	{
		aether::AddDebugLine(*v, ToGlm(a), ToGlm(b), ToGlm(color));
	}
}

AE_SCRIPT_API void aether_debug_draw_ray(Vec3 origin, Vec3 dir, Vec4 color)
{
	if (auto* v = ActiveContext().debugVertices)
	{
		aether::AddDebugLine(*v, ToGlm(origin), ToGlm(origin) + ToGlm(dir), ToGlm(color));
	}
}

AE_SCRIPT_API void aether_debug_draw_sphere(Vec3 center, float radius, Vec4 color)
{
	if (auto* v = ActiveContext().debugVertices)
	{
		aether::AddDebugSphere(*v, ToGlm(center), radius, ToGlm(color));
	}
}

AE_SCRIPT_API void aether_debug_draw_box(Vec3 center, Vec3 halfExtents, Vec4 color)
{
	if (auto* v = ActiveContext().debugVertices)
	{
		const glm::vec3 c = ToGlm(center);
		const glm::vec3 h = ToGlm(halfExtents);
		aether::AddDebugAabb(*v, c - h, c + h, ToGlm(color));
	}
}
