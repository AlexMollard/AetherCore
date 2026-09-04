#include "scripting/interop/InteropCommon.hpp"

#include <utility>

#include "rendering/CustomPassRegistry.hpp"
#include "utils/ServiceContainer.hpp"

using namespace aether::app::scripting;
using namespace aether::app::scripting::interop;

// Project custom render passes: a script registers a pass (name -> project shader + graph stage)
// once, then each frame submits a float4 data buffer + params/colours. The engine runs the named
// shader over a fullscreen pass at the chosen stage and never inspects the data - the meaning lives
// entirely in the project's shader + scripts. See rendering/CustomPassRegistry.hpp.
namespace
{
	// Caps the per-pass buffer so a runaway script can't blow up the fullscreen shader's work.
	constexpr int kMaxVec4 = 4096;

	aether::CustomPassRegistry* Registry() noexcept
	{
		auto* services = ActiveContext().services;
		return services != nullptr ? services->TryGet<aether::CustomPassRegistry>() : nullptr;
	}
} // namespace

AE_SCRIPT_API void aether_custompass_register(const char* name, const char* shader, int stage)
{
	SafeExport([&] -> void
	{
	auto* reg = Registry();
	if (reg == nullptr || name == nullptr)
	{
		return;
	}
	reg->registered[name] = {shader != nullptr ? shader : "", static_cast<aether::CustomPassStage>(stage)};
	});
}

AE_SCRIPT_API void aether_custompass_unregister(const char* name)
{
	SafeExport([&] -> void
	{
	if (auto* reg = Registry(); reg != nullptr && name != nullptr)
	{
		reg->registered.erase(name);
	}
	});
}

AE_SCRIPT_API void aether_custompass_submit(const char* name, const Vec4* data, int count, Vec4 params, Vec4 color0, Vec4 color1)
{
	SafeExport([&] -> void
	{
	auto* reg = Registry();
	if (reg == nullptr || name == nullptr)
	{
		return;
	}
	const auto it = reg->registered.find(name);
	if (it == reg->registered.end())
	{
		return; // submitting to an unregistered pass is a no-op
	}

	aether::CustomPassDraw draw;
	draw.shader = it->second.shader;
	draw.stage = it->second.stage;
	draw.params = ToGlm(params);
	draw.color0 = ToGlm(color0);
	draw.color1 = ToGlm(color1);

	const int n = count < 0 ? 0 : (count > kMaxVec4 ? kMaxVec4 : count);
	if (n > 0 && data != nullptr)
	{
		draw.data.resize(static_cast<std::size_t>(n));
		for (int i = 0; i < n; ++i)
		{
			draw.data[static_cast<std::size_t>(i)] = ToGlm(data[i]);
		}
	}
	reg->frame.passes.push_back(std::move(draw));
	});
}
