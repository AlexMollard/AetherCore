#include "scripting/interop/InteropCommon.hpp"

#include "rendering/InkField.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// Conjured-ink field: scripts rebuild the live stroke each frame by clearing and re-pushing
// segments, which PrepareFrame snapshots into the render packet for the one-pass ink SDF layer
// (see rendering/InkField.hpp + shaders/ink_field.slang).
namespace
{
	// Protects the fullscreen SDF loop (which iterates every segment per pixel) from a runaway
	// stroke. Well above any reasonable amount of on-screen ink.
	constexpr std::size_t kMaxSegments = 512;

	aether::InkField* Field() noexcept
	{
		auto* services = ActiveContext().services;
		return services != nullptr ? services->TryGet<aether::InkField>() : nullptr;
	}
} // namespace

AE_SCRIPT_API void aether_ink_clear()
{
	if (auto* f = Field())
	{
		f->data.segments.clear();
	}
}

AE_SCRIPT_API void aether_ink_set_colors(Vec4 body, Vec4 rim)
{
	if (auto* f = Field())
	{
		f->data.bodyColor = ToGlm(body);
		f->data.rimColor = ToGlm(rim);
	}
}

AE_SCRIPT_API void aether_ink_add_segment(float ax, float ay, float bx, float by, float width, float alpha, float glow, float ghost)
{
	auto* f = Field();
	if (f == nullptr || f->data.segments.size() >= kMaxSegments)
	{
		return;
	}
	f->data.segments.push_back(aether::InkSegmentGpu{{ax, ay}, {bx, by}, width, alpha, glow, ghost});
}
