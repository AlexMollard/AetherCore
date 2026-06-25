#pragma once

// Engine-side Tracy GPU profiling facade.
//
// Vulkan-free. No token with `Vk` in its name appears in this header.
// The implementation lives in `src/engine/vulkan/GpuProfiler.cpp`,
// which is the only translation unit that includes vulkan +
// TracyVulkan headers and performs the `reinterpret_cast<VkCommandBuffer>`.
//
// A pImpl pattern hides Tracy's types behind forward-declared
// `detail::*` structs. The engine side only sees `aether::gpu::*`
// types. The factory that builds the impl lives in vulkan/.
//
// A singleton holds the context. Engine code uses
// `AE_GPU_ZONE_SCOPED(cmd, name)` for scoped zones and
// `GpuProfiler::Get().Collect(cmd)` for frame collection. Source
// location is captured at the call site via the macro.

#include <cstdint>
#include <string_view>

#include "gpu/GpuTypes.hpp"

#ifndef AETHERCORE_ENABLE_TRACY_GPU
#	define AETHERCORE_ENABLE_TRACY_GPU 1
#endif

namespace aether::gpu::detail
{
	// Forward-declared pImpl data. Full definitions live in
	// `vulkan/GpuProfiler.cpp`. The engine side never sees the
	// real type names.
	struct ProfilerContextData;
	struct ProfilerScopeData;
} // namespace aether::gpu::detail

namespace aether::gpu
{
	// Opaque handles to the pImpl data. The caller in `vulkan/`
	// obtains a `ProfilerContextHandle` from
	// `aether::vulkan::CreateTracyGpuProfilerContext(...)` and
	// passes it to `GpuProfiler::Initialize`. The engine never
	// sees a `tracy::*` or `Vk*` token.
	using ProfilerContextHandle = detail::ProfilerContextData*;
	using ProfilerScopeHandle = detail::ProfilerScopeData*;

	// POD initialization data. The `context` field is built by the
	// vulkan-side factory; the engine side only stores it.
	struct GpuProfilerInit
	{
		ProfilerContextHandle context = nullptr;
	};

	// RAII guard for a scoped GPU zone. Ends the underlying Tracy
	// zone on destruction. The handle is opaque: the engine side
	// has no knowledge of Tracy types.
	//
	// Construction is via `GpuProfiler::BeginZoneScopedImpl` (a member
	// function) so the ctor can stay private. Callers use the
	// `AE_GPU_ZONE_SCOPED` macro, which expands to a declaration that
	// binds the return value of the factory function - no public ctor
	// is needed.
	class GpuZoneScope
	{
	public:
		GpuZoneScope() noexcept = default;
		GpuZoneScope(const GpuZoneScope&) = delete;
		GpuZoneScope& operator=(const GpuZoneScope&) = delete;

		GpuZoneScope(GpuZoneScope&& o) noexcept
		      : m_handle(o.m_handle)
		{
			o.m_handle = nullptr;
		}

		GpuZoneScope& operator=(GpuZoneScope&& o) noexcept
		{
			if (this != &o)
			{
				Reset();
				m_handle = o.m_handle;
				o.m_handle = nullptr;
			}
			return *this;
		}

		~GpuZoneScope()
		{
			Reset();
		}

		void Reset() noexcept;

	private:
		friend class GpuProfiler;

		explicit GpuZoneScope(ProfilerScopeHandle handle) noexcept
		      : m_handle(handle)
		{
		}

		ProfilerScopeHandle m_handle = nullptr;
	};

	// Singleton wrapper. Stores the Tracy context behind a typed
	// pImpl handle.
	class GpuProfiler
	{
	public:
		static GpuProfiler& Get() noexcept;

		void Initialize(const GpuProfilerInit& init) noexcept;
		void Shutdown() noexcept;

		// Set the human-readable name shown in Tracy's timeline.
		// Trivial no-op when the context isn't initialized.
		void SetName(std::string_view name) noexcept;

		// Source location is passed in by the `AE_GPU_ZONE_SCOPED`
		// macro. Returns a `GpuZoneScope` ready to use.
		GpuZoneScope BeginZoneScopedImpl(gpu::CommandBuffer cmd, std::string_view name, const char* file, std::uint32_t line, const char* func) noexcept;

		void Collect(gpu::CommandBuffer cmd) noexcept;

	private:
		ProfilerContextHandle m_context = nullptr;
	};
} // namespace aether::gpu

#define AE_GPU_PROFILER_CONCAT_INNER(a, b) a##b
#define AE_GPU_PROFILER_CONCAT(a, b) AE_GPU_PROFILER_CONCAT_INNER(a, b)

// Scoped GPU zone. Captures __FILE__ / __LINE__ at the call site so
// the zone is attributed to the caller, not GpuProfiler.cpp. Contains
// no `Vk*` token; the cast lives in vulkan/GpuProfiler.cpp.
//
// Implementation note: the macro declares a local with the return
// value of `GpuProfiler::BeginZoneScopedImpl`, which is a member
// function. The function constructs the private `GpuZoneScope`, so
// no public ctor is needed on `GpuZoneScope` and callers cannot
// forge a scope handle themselves.
#if AETHERCORE_ENABLE_TRACY_GPU
#	define AE_GPU_ZONE_SCOPED(cmd, name) \
		auto AE_GPU_PROFILER_CONCAT(_ae_gpu_zone_, __LINE__) = \
			::aether::gpu::GpuProfiler::Get().BeginZoneScopedImpl( \
				(cmd), (name), __FILE__, __LINE__, __func__)
#else
#	define AE_GPU_ZONE_SCOPED(cmd, name) (void) 0
#endif
