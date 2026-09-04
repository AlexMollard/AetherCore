#include "scripting/interop/InteropCommon.hpp"

#include "rendering/Light2DSubmission.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// Transient 2D lights and shadow occluders submitted per frame by scripts, for effects with no ECS
// entity behind them (a drawn ink stroke, a projectile trail). Submissions last exactly one frame:
// PrepareFrame drains them into the render packet and clears, so a script that stops submitting stops
// lighting. The engine stays game-agnostic - it only knows "a light here", "a capsule occludes here".
// See rendering/Light2DSubmission.hpp.
namespace
{
	// Caps so a runaway script can't tank the per-pixel light loop / occluder pass.
	constexpr int kMaxLights = 64;
	constexpr int kMaxOccluders = 1024;

	aether::Light2DSubmissionRegistry* Registry() noexcept
	{
		auto* services = ActiveContext().services;
		return services != nullptr ? services->TryGet<aether::Light2DSubmissionRegistry>() : nullptr;
	}
} // namespace

AE_SCRIPT_API void aether_light2d_submit_light(float x, float y, float radius, Vec3 color, float intensity, int castsShadow)
{
	SafeExport([&] -> void
	{
	auto* reg = Registry();
	if (reg == nullptr || static_cast<int>(reg->lights.size()) >= kMaxLights || radius <= 0.0f)
	{
		return;
	}
	reg->lights.push_back(aether::Renderer::PointLight{
	        .position = glm::vec3(x, y, 0.0f),
	        .radius = radius,
	        .color = ToGlm(color),
	        .intensity = intensity,
	        .castsShadow = castsShadow != 0,
	});
	});
}

// A capsule occluder: the segment a->b thickened by `radius`. Shadow-only; it does not draw anything.
AE_SCRIPT_API void aether_light2d_submit_occluder_capsule(float ax, float ay, float bx, float by, float radius)
{
	SafeExport([&] -> void
	{
	auto* reg = Registry();
	if (reg == nullptr || static_cast<int>(reg->occluders.size()) >= kMaxOccluders || radius <= 0.0f)
	{
		return;
	}
	aether::Occluder2D occ;
	occ.posHalfSize = glm::vec4(ax, ay, radius, 0.0f);
	occ.uvRect = glm::vec4(bx, by, 0.0f, 0.0f);
	occ.textureIndex = 0;
	occ.flags = aether::kOccluder2DCapsule;
	reg->occluders.push_back(occ);
	});
}
