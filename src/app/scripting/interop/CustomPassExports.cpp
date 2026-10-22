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
	reg->Register(name, shader != nullptr ? shader : "", static_cast<aether::CustomPassStage>(stage));
	});
}

AE_SCRIPT_API void aether_custompass_unregister(const char* name)
{
	SafeExport([&] -> void
	{
	if (auto* reg = Registry(); reg != nullptr && name != nullptr)
	{
		reg->Unregister(name);
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
	const std::size_t n = data != nullptr && count > 0 ? static_cast<std::size_t>(count) : 0;
	// Submitting to an unregistered pass is a no-op.
	reg->SubmitIndexed(name, n, [&](std::size_t i) { return ToGlm(data[i]); }, ToGlm(params), ToGlm(color0), ToGlm(color1));
	});
}
