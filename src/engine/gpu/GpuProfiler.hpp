#pragma once

#include <cstdint>
#include <string_view>

#include "gpu/GpuTypes.hpp"

#ifndef AETHERCORE_ENABLE_TRACY_GPU
#	define AETHERCORE_ENABLE_TRACY_GPU 1
#endif

namespace aether::gpu::detail
{
	// `vulkan/GpuProfiler.cpp`. The engine side never sees the
	struct ProfilerContextData;
	struct ProfilerScopeData;
} // namespace aether::gpu::detail

namespace aether::gpu
{
	// passes it to `GpuProfiler::Initialize`. The engine never
	using ProfilerContextHandle = detail::ProfilerContextData*;
	using ProfilerScopeHandle = detail::ProfilerScopeData*;

	struct GpuProfilerInit
	{
		ProfilerContextHandle context = nullptr;
	};

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

	class GpuProfiler
	{
	public:
		static GpuProfiler& Get() noexcept;

		void Initialize(const GpuProfilerInit& init) noexcept;
		void Shutdown() noexcept;

		void SetName(std::string_view name) noexcept;

		GpuZoneScope BeginZoneScopedImpl(gpu::CommandBuffer cmd, std::string_view name, const char* file, std::uint32_t line, const char* func) noexcept;

		void Collect(gpu::CommandBuffer cmd) noexcept;

	private:
		ProfilerContextHandle m_context = nullptr;
	};
} // namespace aether::gpu

#define AE_GPU_PROFILER_CONCAT_INNER(a, b) a##b
#define AE_GPU_PROFILER_CONCAT(a, b) AE_GPU_PROFILER_CONCAT_INNER(a, b)

#if AETHERCORE_ENABLE_TRACY_GPU
#	define AE_GPU_ZONE_SCOPED(cmd, name) \
		auto AE_GPU_PROFILER_CONCAT(_ae_gpu_zone_, __LINE__) = \
			::aether::gpu::GpuProfiler::Get().BeginZoneScopedImpl( \
				(cmd), (name), __FILE__, __LINE__, __func__)
#else
#	define AE_GPU_ZONE_SCOPED(cmd, name) (void) 0
#endif
